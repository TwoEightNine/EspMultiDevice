#pragma once

#include <stddef.h>
#include <stdint.h>

// Incremental JSON parser that keeps only selected object members. Bytes
// may arrive in pieces of any size, so a large HTTP body is filtered as it
// streams instead of being buffered. Kept values become a compact pre-order
// node array plus one string arena, both owned by this object and sized by
// the caller's limits. The parser validates the whole document, including
// members it drops, and rejects invalid UTF-8.
class JsonPick {
public:
  enum class Type : uint8_t { Null, False, True, Number, String, Array, Object };
  static constexpr uint32_t NoKey = 0xffffffffu;
  static constexpr uint8_t MaxKeepKeys = 32;
  static constexpr uint8_t MaxKeyBytes = 64;

  // A container's `count` is its kept children and `end` the index after its
  // last descendant. A scalar's text is `count` bytes at `offset`.
  struct Node {
    uint32_t keyOffset;
    uint16_t keyLength;
    Type type;
    uint8_t reserved;
    uint32_t offset;  // scalar text offset; unused for containers
    union {
      uint32_t count;  // container: kept children; scalar: text length
      uint32_t length;
    };
    uint32_t end;
  };

  struct Options {
    // Keys kept at any depth; none means keep every member.
    const char *keep[MaxKeepKeys] = {};
    uint8_t keepCount = 0;
    uint32_t maxString = 0;   // 0: no per-string cap
    uint32_t maxItems = 0;    // 0: no per-array cap
    uint32_t maxNodes = 8000;
    uint32_t maxStringBytes = 128 * 1024;
    uint8_t maxDepth = 16;
  };

  using Allocate = void *(*)(size_t bytes);
  using Release = void (*)(void *pointer);

  JsonPick(Allocate allocate, Release release);
  ~JsonPick();
  JsonPick(const JsonPick &) = delete;
  JsonPick &operator=(const JsonPick &) = delete;

  bool begin(const Options &options);
  bool feed(const uint8_t *data, size_t length);
  bool finish();
  void reset();

  const char *error() const { return _error; }
  bool truncated() const { return _truncated; }
  const Node *nodes() const { return _nodes; }
  uint32_t nodeCount() const { return _nodeCount; }
  const char *strings() const { return _strings; }
  uint32_t stringBytes() const { return _stringBytes; }

private:
  enum class State : uint8_t {
    Value, ValueOrEnd, Element, KeyOrEnd, Key, Colon, After, String, Number, Literal, Done
  };
  struct Frame {
    bool object;
    bool kept;
    uint32_t node;   // kept containers only
    uint32_t items;  // array elements seen, kept or not
  };
  static constexpr uint8_t MaxSkipDepth = 64;

  Allocate _allocate;
  Release _release;
  Node *_nodes = nullptr;
  char *_strings = nullptr;
  uint32_t _nodeCount = 0;
  uint32_t _stringBytes = 0;
  Options _options{};
  char _keepText[MaxKeepKeys][MaxKeyBytes + 1] = {};
  uint8_t _keepLengths[MaxKeepKeys] = {};
  Frame _stack[MaxSkipDepth] = {};
  uint8_t _depth = 0;
  uint8_t _keptDepth = 0;
  State _state = State::Value;
  bool _rootSeen = false;
  bool _truncated = false;
  bool _bomChecked = false;
  uint8_t _bomBytes = 0;
  const char *_error = nullptr;

  // The value about to start.
  bool _pendingKept = false;
  // Key being read, and whether the string being read is a key.
  char _key[MaxKeyBytes];
  uint8_t _keyLength = 0;
  bool _keyTooLong = false;
  bool _stringIsKey = false;
  // String value being stored.
  uint32_t _valueNode = 0;
  bool _storeString = false;
  bool _stringFull = false;
  uint8_t _escape = 0;        // 0 none, 1 after '\', 2..5 reading \u digits
  uint32_t _unicode = 0;
  uint32_t _highSurrogate = 0;
  bool _expectLowSurrogate = false;
  uint8_t _utf8[4] = {};
  uint8_t _utf8Have = 0;
  uint8_t _utf8Need = 0;
  // Number or literal being read.
  char _token[40];
  uint8_t _tokenLength = 0;
  const char *_literal = nullptr;
  uint8_t _literalLength = 0;
  Type _literalType = Type::Null;

  bool fail(const char *message);
  bool step(uint8_t c);
  bool startValue(uint8_t c);
  bool finishScalar(Type type, const char *text, uint32_t length);
  bool endValue();
  bool pushContainer(bool object);
  bool popContainer(bool object);
  bool keyKept() const;
  bool newNode(Type type, uint32_t &index);
  bool appendString(const char *data, uint32_t length, uint32_t &offset);
  bool emitCodepoint(uint32_t codepoint);
  bool emitBytes(const uint8_t *bytes, uint8_t length);
  bool stringByte(uint8_t c);
  bool validNumber() const;
};
