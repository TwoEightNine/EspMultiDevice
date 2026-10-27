#include "JsonPick.h"

#include <string.h>

namespace {
bool whitespace(uint8_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool hexDigit(uint8_t c, uint32_t &value) {
  if (c >= '0' && c <= '9') value = c - '0';
  else if (c >= 'a' && c <= 'f') value = c - 'a' + 10;
  else if (c >= 'A' && c <= 'F') value = c - 'A' + 10;
  else return false;
  return true;
}
}

JsonPick::JsonPick(Allocate allocate, Release release) : _allocate(allocate), _release(release) {}

JsonPick::~JsonPick() { reset(); }

void JsonPick::reset() {
  if (_nodes) _release(_nodes);
  if (_strings) _release(_strings);
  _nodes = nullptr;
  _strings = nullptr;
  _nodeCount = _stringBytes = 0;
  _error = nullptr;
}

bool JsonPick::begin(const Options &options) {
  reset();
  _options = options;
  if (_options.keepCount > MaxKeepKeys) _options.keepCount = MaxKeepKeys;
  if (_options.maxDepth == 0 || _options.maxDepth > MaxSkipDepth) _options.maxDepth = 16;
  if (_options.maxNodes == 0) _options.maxNodes = 1;
  for (uint8_t i = 0; i < _options.keepCount; ++i) {
    const char *key = _options.keep[i] ? _options.keep[i] : "";
    const size_t length = strnlen(key, MaxKeyBytes + 1);
    _keepLengths[i] = length > MaxKeyBytes ? 0 : static_cast<uint8_t>(length);
    memcpy(_keepText[i], key, _keepLengths[i]);
    _keepText[i][_keepLengths[i]] = 0;
    _options.keep[i] = _keepText[i];
  }
  _nodes = static_cast<Node *>(_allocate(sizeof(Node) * _options.maxNodes));
  _strings = static_cast<char *>(_allocate(_options.maxStringBytes + 1));
  if (!_nodes || !_strings) { fail("Out of memory"); return false; }
  _depth = _keptDepth = 0;
  _state = State::Value;
  _rootSeen = _truncated = _bomChecked = false;
  _bomBytes = 0;
  _pendingKept = true;
  _escape = 0;
  _expectLowSurrogate = false;
  _utf8Have = _utf8Need = 0;
  _tokenLength = 0;
  _error = nullptr;
  return true;
}

bool JsonPick::fail(const char *message) {
  if (!_error) _error = message;
  return false;
}

bool JsonPick::feed(const uint8_t *data, size_t length) {
  if (_error) return false;
  if (!_nodes) return fail("Out of memory");
  for (size_t i = 0; i < length; ++i) {
    const uint8_t c = data[i];
    if (!_bomChecked) {
      static const uint8_t bom[3] = {0xEF, 0xBB, 0xBF};
      if (c == bom[_bomBytes]) {
        if (++_bomBytes == 3) _bomChecked = true;
        continue;
      }
      if (_bomBytes) return fail("Invalid JSON");
      _bomChecked = true;
    }
    if (!step(c)) return false;
  }
  return true;
}

bool JsonPick::finish() {
  if (_error) return false;
  if (_state == State::Number) {
    if (!validNumber()) return fail("Invalid JSON");
    if (!finishScalar(Type::Number, _token, _tokenLength) || !endValue()) return false;
  }
  if (_state != State::Done) return fail("Invalid JSON");
  return true;
}

bool JsonPick::keyKept() const {
  if (_options.keepCount == 0) return true;
  if (_keyTooLong) return false;
  for (uint8_t i = 0; i < _options.keepCount; ++i) {
    if (_keepLengths[i] == _keyLength && memcmp(_keepText[i], _key, _keyLength) == 0) return true;
  }
  return false;
}

bool JsonPick::appendString(const char *data, uint32_t length, uint32_t &offset) {
  if (length > _options.maxStringBytes - _stringBytes) return fail("JSON too large");
  offset = _stringBytes;
  memcpy(_strings + _stringBytes, data, length);
  _stringBytes += length;
  return true;
}

bool JsonPick::newNode(Type type, uint32_t &index) {
  if (_nodeCount >= _options.maxNodes) return fail("JSON too large");
  Node node{};
  node.keyOffset = NoKey;
  node.type = type;
  if (_depth > 0 && _stack[_depth - 1].kept) {
    Frame &parent = _stack[_depth - 1];
    if (parent.object) {
      uint32_t keyOffset = 0;
      if (!appendString(_key, _keyLength, keyOffset)) return false;
      node.keyOffset = keyOffset;
      node.keyLength = _keyLength;
    }
    ++_nodes[parent.node].count;
  }
  node.offset = _stringBytes;
  node.count = 0;
  index = _nodeCount++;
  node.end = _nodeCount;
  _nodes[index] = node;
  return true;
}

bool JsonPick::pushContainer(bool object) {
  if (_depth >= MaxSkipDepth) return fail("JSON too deep");
  Frame frame{object, _pendingKept, 0, 0};
  if (frame.kept) {
    if (_keptDepth >= _options.maxDepth) return fail("JSON too deep");
    if (!newNode(object ? Type::Object : Type::Array, frame.node)) return false;
    ++_keptDepth;
  }
  _stack[_depth++] = frame;
  _state = object ? State::KeyOrEnd : State::ValueOrEnd;
  return true;
}

bool JsonPick::popContainer(bool object) {
  if (!_depth || _stack[_depth - 1].object != object) return fail("Invalid JSON");
  const Frame &frame = _stack[--_depth];
  if (frame.kept) {
    _nodes[frame.node].end = _nodeCount;
    --_keptDepth;
  }
  return endValue();
}

bool JsonPick::endValue() {
  _state = _depth == 0 ? State::Done : State::After;
  if (_depth == 0) _rootSeen = true;
  return true;
}

bool JsonPick::finishScalar(Type type, const char *text, uint32_t length) {
  if (!_pendingKept) return true;
  uint32_t index = 0;
  if (!newNode(type, index)) return false;
  if (length) {
    uint32_t offset = 0;
    if (!appendString(text, length, offset)) return false;
    _nodes[index].offset = offset;
    _nodes[index].length = length;
  }
  return true;
}

bool JsonPick::emitBytes(const uint8_t *bytes, uint8_t length) {
  if (_stringIsKey) {
    if (_keyLength + length > MaxKeyBytes) _keyTooLong = true;
    else { memcpy(_key + _keyLength, bytes, length); _keyLength += length; }
    return true;
  }
  if (!_storeString || _stringFull) return true;
  Node &node = _nodes[_valueNode];
  if (_options.maxString && node.length + length > _options.maxString) {
    _stringFull = true;
    _truncated = true;
    return true;
  }
  uint32_t offset = 0;
  if (!appendString(reinterpret_cast<const char *>(bytes), length, offset)) return false;
  node.length += length;
  return true;
}

bool JsonPick::emitCodepoint(uint32_t codepoint) {
  uint8_t bytes[4];
  uint8_t length = 0;
  if (codepoint < 0x80) {
    bytes[length++] = static_cast<uint8_t>(codepoint);
  } else if (codepoint < 0x800) {
    bytes[length++] = static_cast<uint8_t>(0xC0 | (codepoint >> 6));
    bytes[length++] = static_cast<uint8_t>(0x80 | (codepoint & 0x3F));
  } else if (codepoint < 0x10000) {
    bytes[length++] = static_cast<uint8_t>(0xE0 | (codepoint >> 12));
    bytes[length++] = static_cast<uint8_t>(0x80 | ((codepoint >> 6) & 0x3F));
    bytes[length++] = static_cast<uint8_t>(0x80 | (codepoint & 0x3F));
  } else {
    bytes[length++] = static_cast<uint8_t>(0xF0 | (codepoint >> 18));
    bytes[length++] = static_cast<uint8_t>(0x80 | ((codepoint >> 12) & 0x3F));
    bytes[length++] = static_cast<uint8_t>(0x80 | ((codepoint >> 6) & 0x3F));
    bytes[length++] = static_cast<uint8_t>(0x80 | (codepoint & 0x3F));
  }
  return emitBytes(bytes, length);
}

bool JsonPick::stringByte(uint8_t c) {
  if (_escape == 1) {
    _escape = 0;
    if (_expectLowSurrogate && c != 'u') return fail("Invalid JSON");
    switch (c) {
      case '"': case '\\': case '/': return emitBytes(&c, 1);
      case 'b': return emitCodepoint('\b');
      case 'f': return emitCodepoint('\f');
      case 'n': return emitCodepoint('\n');
      case 'r': return emitCodepoint('\r');
      case 't': return emitCodepoint('\t');
      case 'u': _escape = 2; _unicode = 0; return true;
      default: return fail("Invalid JSON");
    }
  }
  if (_escape >= 2) {
    uint32_t digit = 0;
    if (!hexDigit(c, digit)) return fail("Invalid JSON");
    _unicode = (_unicode << 4) | digit;
    if (++_escape < 6) return true;
    _escape = 0;
    if (_expectLowSurrogate) {
      if (_unicode < 0xDC00 || _unicode > 0xDFFF) return fail("Invalid JSON");
      _expectLowSurrogate = false;
      return emitCodepoint(0x10000 + ((_highSurrogate - 0xD800) << 10) + (_unicode - 0xDC00));
    }
    if (_unicode >= 0xD800 && _unicode <= 0xDBFF) {
      _highSurrogate = _unicode;
      _expectLowSurrogate = true;
      return true;
    }
    if (_unicode >= 0xDC00 && _unicode <= 0xDFFF) return fail("Invalid JSON");
    return emitCodepoint(_unicode);
  }
  if (_expectLowSurrogate && c != '\\') return fail("Invalid JSON");
  if (_utf8Need) {
    uint8_t low = 0x80, high = 0xBF;
    if (_utf8Have == 1) {
      if (_utf8[0] == 0xE0) low = 0xA0;
      else if (_utf8[0] == 0xED) high = 0x9F;
      else if (_utf8[0] == 0xF0) low = 0x90;
      else if (_utf8[0] == 0xF4) high = 0x8F;
    }
    if (c < low || c > high) return fail("Invalid UTF-8");
    _utf8[_utf8Have++] = c;
    if (--_utf8Need) return true;
    const uint8_t length = _utf8Have;
    _utf8Have = 0;
    return emitBytes(_utf8, length);
  }
  if (c == '"') {
    if (_stringIsKey) { _state = State::Colon; return true; }
    return endValue();
  }
  if (c == '\\') { _escape = 1; return true; }
  if (c < 0x20) return fail("Invalid JSON");
  if (c < 0x80) return emitBytes(&c, 1);
  if (c >= 0xC2 && c <= 0xDF) _utf8Need = 1;
  else if (c >= 0xE0 && c <= 0xEF) _utf8Need = 2;
  else if (c >= 0xF0 && c <= 0xF4) _utf8Need = 3;
  else return fail("Invalid UTF-8");
  _utf8[0] = c;
  _utf8Have = 1;
  return true;
}

bool JsonPick::validNumber() const {
  uint8_t i = 0;
  const uint8_t n = _tokenLength;
  if (i < n && _token[i] == '-') ++i;
  if (i >= n) return false;
  if (_token[i] == '0') ++i;
  else if (_token[i] >= '1' && _token[i] <= '9') { while (i < n && _token[i] >= '0' && _token[i] <= '9') ++i; }
  else return false;
  if (i < n && _token[i] == '.') {
    ++i;
    if (i >= n || _token[i] < '0' || _token[i] > '9') return false;
    while (i < n && _token[i] >= '0' && _token[i] <= '9') ++i;
  }
  if (i < n && (_token[i] == 'e' || _token[i] == 'E')) {
    ++i;
    if (i < n && (_token[i] == '+' || _token[i] == '-')) ++i;
    if (i >= n || _token[i] < '0' || _token[i] > '9') return false;
    while (i < n && _token[i] >= '0' && _token[i] <= '9') ++i;
  }
  return i == n;
}

bool JsonPick::startValue(uint8_t c) {
  if (c == '{') return pushContainer(true);
  if (c == '[') return pushContainer(false);
  if (c == '"') {
    _stringIsKey = false;
    _storeString = _pendingKept;
    _stringFull = false;
    _escape = 0;
    _expectLowSurrogate = false;
    _utf8Have = _utf8Need = 0;
    if (_storeString && !newNode(Type::String, _valueNode)) return false;
    _state = State::String;
    return true;
  }
  if (c == '-' || (c >= '0' && c <= '9')) {
    _token[0] = static_cast<char>(c);
    _tokenLength = 1;
    _state = State::Number;
    return true;
  }
  if (c == 't') { _literal = "true"; _literalType = Type::True; }
  else if (c == 'f') { _literal = "false"; _literalType = Type::False; }
  else if (c == 'n') { _literal = "null"; _literalType = Type::Null; }
  else return fail("Invalid JSON");
  _literalLength = 1;
  _state = State::Literal;
  return true;
}

bool JsonPick::step(uint8_t c) {
  switch (_state) {
    case State::String:
      return stringByte(c);
    case State::Number:
      if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') {
        if (_tokenLength >= sizeof(_token) - 1) return fail("Invalid JSON");
        _token[_tokenLength++] = static_cast<char>(c);
        return true;
      }
      if (!validNumber()) return fail("Invalid JSON");
      if (!finishScalar(Type::Number, _token, _tokenLength) || !endValue()) return false;
      return step(c);
    case State::Literal:
      if (c != static_cast<uint8_t>(_literal[_literalLength])) return fail("Invalid JSON");
      if (_literal[++_literalLength]) return true;
      if (!finishScalar(_literalType, nullptr, 0)) return false;
      return endValue();
    default:
      break;
  }
  if (whitespace(c)) return true;
  switch (_state) {
    case State::Value:
      if (_depth == 0) {
        if (_rootSeen) return fail("Invalid JSON");
        _pendingKept = true;
      }
      return startValue(c);
    case State::After: {
      const Frame &frame = _stack[_depth - 1];
      if (c == '}' || c == ']') return popContainer(c == '}');
      if (c != ',') return fail("Invalid JSON");
      _state = frame.object ? State::Key : State::Element;
      return true;
    }
    case State::ValueOrEnd:
    case State::Element: {
      if (c == ']' && _state == State::ValueOrEnd) return popContainer(false);
      Frame &frame = _stack[_depth - 1];
      _pendingKept = frame.kept && (_options.maxItems == 0 || frame.items < _options.maxItems);
      if (frame.kept && !_pendingKept) _truncated = true;
      ++frame.items;
      return startValue(c);
    }
    case State::KeyOrEnd:
      if (c == '}') return popContainer(true);
      // fallthrough
    case State::Key:
      if (c != '"') return fail("Invalid JSON");
      _stringIsKey = true;
      _keyLength = 0;
      _keyTooLong = false;
      _escape = 0;
      _expectLowSurrogate = false;
      _utf8Have = _utf8Need = 0;
      _state = State::String;
      return true;
    case State::Colon:
      if (c != ':') return fail("Invalid JSON");
      _pendingKept = _stack[_depth - 1].kept && keyKept();
      _state = State::Value;
      return true;
    case State::Done:
      return fail("Invalid JSON");
    default:
      return fail("Invalid JSON");
  }
}
