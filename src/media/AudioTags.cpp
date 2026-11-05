#include "AudioTags.h"

#include <string.h>

namespace AudioTags {
namespace {

enum Field : uint8_t { TitleField = 0, ArtistField = 1, AlbumField = 2, YearField = 3 };

constexpr uint32_t MaxUnsyncSkipBytes = 32 * 1024;

uint32_t readBe32(const uint8_t *bytes) {
  return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
         ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

uint32_t readLe32(const uint8_t *bytes) {
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
         ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

bool readSynchsafe(const uint8_t *bytes, uint32_t &value) {
  if ((bytes[0] | bytes[1] | bytes[2] | bytes[3]) & 0x80) return false;
  value = ((uint32_t)bytes[0] << 21) | ((uint32_t)bytes[1] << 14) |
          ((uint32_t)bytes[2] << 7) | (uint32_t)bytes[3];
  return true;
}

bool isFrameIdChar(uint8_t c) {
  return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// Removes the 0x00 that unsynchronisation inserts after every 0xFF.
size_t deunsync(uint8_t *data, size_t length) {
  size_t out = 0;
  bool previousFf = false;
  for (size_t i = 0; i < length; i++) {
    uint8_t b = data[i];
    if (previousFf && b == 0x00) {
      previousFf = false;
      continue;
    }
    data[out++] = b;
    previousFf = b == 0xff;
  }
  return out;
}

// ---- Text decoding -------------------------------------------------------

class Utf8Writer {
public:
  Utf8Writer(char *out, size_t outBytes)
    : _out(out), _capacity(outBytes > 0 ? outBytes - 1 : 0), _length(0), _trimmed(0), _full(false) {
    if (outBytes > 0) out[0] = '\0';
  }

  // Returns false once the output is full.
  bool put(uint32_t cp) {
    if (_full) return false;
    if (cp < 0x20 || cp == 0x7f || (cp >= 0x80 && cp < 0xa0)) cp = ' ';
    if (cp == ' ' && _length == 0) return true;  // trim leading space
    uint8_t bytes[4];
    size_t count;
    if (cp < 0x80) {
      bytes[0] = (uint8_t)cp;
      count = 1;
    } else if (cp < 0x800) {
      bytes[0] = (uint8_t)(0xc0 | (cp >> 6));
      bytes[1] = (uint8_t)(0x80 | (cp & 0x3f));
      count = 2;
    } else if (cp < 0x10000) {
      bytes[0] = (uint8_t)(0xe0 | (cp >> 12));
      bytes[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f));
      bytes[2] = (uint8_t)(0x80 | (cp & 0x3f));
      count = 3;
    } else {
      bytes[0] = (uint8_t)(0xf0 | (cp >> 18));
      bytes[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3f));
      bytes[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f));
      bytes[3] = (uint8_t)(0x80 | (cp & 0x3f));
      count = 4;
    }
    if (_length + count > _capacity) {
      _full = true;
      return false;
    }
    memcpy(_out + _length, bytes, count);
    _length += count;
    if (cp != ' ') _trimmed = _length;
    return true;
  }

  size_t finish() {
    // Trailing spaces are dropped; everything else ends on a whole character.
    _out[_trimmed] = '\0';
    return _trimmed;
  }

private:
  char *_out;
  size_t _capacity;
  size_t _length;
  size_t _trimmed;
  bool _full;
};

// Decodes one UTF-8 sequence; invalid input yields '?' and consumes one byte.
uint32_t nextUtf8(const uint8_t *data, size_t length, size_t &i) {
  uint8_t b = data[i];
  if (b < 0x80) {
    i++;
    return b;
  }
  size_t count;
  uint32_t cp;
  uint32_t minimum;
  if ((b & 0xe0) == 0xc0) { count = 1; cp = b & 0x1f; minimum = 0x80; }
  else if ((b & 0xf0) == 0xe0) { count = 2; cp = b & 0x0f; minimum = 0x800; }
  else if ((b & 0xf8) == 0xf0) { count = 3; cp = b & 0x07; minimum = 0x10000; }
  else { i++; return '?'; }
  if (length - i <= count) {  // truncated sequence
    i++;
    return '?';
  }
  for (size_t k = 1; k <= count; k++) {
    uint8_t c = data[i + k];
    if ((c & 0xc0) != 0x80) {
      i++;
      return '?';
    }
    cp = (cp << 6) | (c & 0x3f);
  }
  if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
    i++;
    return '?';
  }
  i += count + 1;
  return cp;
}

bool isValidUtf8WithMultibyte(const uint8_t *data, size_t length) {
  bool multibyte = false;
  size_t i = 0;
  while (i < length) {
    if (data[i] < 0x80) {
      i++;
      continue;
    }
    size_t before = i;
    uint32_t cp = nextUtf8(data, length, i);
    if (cp == '?' && i == before + 1) return false;
    multibyte = true;
  }
  return multibyte;
}

// Windows-1251 Cyrillic letters (А–я, Ё, ё) in a run of at least three high
// bytes. Isolated accented Latin-1 letters ("Beyoncé") stay Latin-1.
bool looksLikeCp1251(const uint8_t *data, size_t length) {
  size_t run = 0;
  size_t longest = 0;
  for (size_t i = 0; i < length; i++) {
    uint8_t b = data[i];
    if (b < 0x80) {
      run = 0;
      continue;
    }
    if (b < 0xc0 && b != 0xa8 && b != 0xb8) return false;
    run++;
    if (run > longest) longest = run;
  }
  return longest >= 3;
}

uint32_t cp1251ToUnicode(uint8_t b) {
  if (b >= 0xc0) return 0x0410 + (b - 0xc0);
  if (b == 0xa8) return 0x0401;
  if (b == 0xb8) return 0x0451;
  return b;
}

}  // namespace

size_t decodeText(const uint8_t *data, size_t length, uint8_t encoding,
                  char *out, size_t outBytes) {
  Utf8Writer writer(out, outBytes);
  if (outBytes == 0) return 0;
  if (encoding == 1 || encoding == 2) {
    bool bigEndian = encoding == 2;
    size_t i = 0;
    if (encoding == 1 && length >= 2) {
      if (data[0] == 0xff && data[1] == 0xfe) { bigEndian = false; i = 2; }
      else if (data[0] == 0xfe && data[1] == 0xff) { bigEndian = true; i = 2; }
    }
    while (i + 1 < length) {
      uint16_t unit = bigEndian ? (uint16_t)((data[i] << 8) | data[i + 1])
                                : (uint16_t)(data[i] | (data[i + 1] << 8));
      i += 2;
      if (unit == 0) break;
      uint32_t cp = unit;
      if (unit >= 0xd800 && unit <= 0xdbff) {
        cp = '?';
        if (i + 1 < length) {
          uint16_t low = bigEndian ? (uint16_t)((data[i] << 8) | data[i + 1])
                                   : (uint16_t)(data[i] | (data[i + 1] << 8));
          if (low >= 0xdc00 && low <= 0xdfff) {
            cp = 0x10000 + (((uint32_t)unit - 0xd800) << 10) + (low - 0xdc00);
            i += 2;
          }
        }
      } else if (unit >= 0xdc00 && unit <= 0xdfff) {
        cp = '?';
      } else if (unit == 0xfeff) {
        continue;  // a BOM repeated before a later value
      }
      if (!writer.put(cp)) break;
    }
    return writer.finish();
  }

  size_t n = 0;
  while (n < length && data[n] != 0) n++;
  if (encoding == 3 || isValidUtf8WithMultibyte(data, n)) {
    size_t i = 0;
    // Skip a UTF-8 BOM.
    if (n >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf) i = 3;
    while (i < n) {
      if (!writer.put(nextUtf8(data, n, i))) break;
    }
    return writer.finish();
  }
  bool cp1251 = looksLikeCp1251(data, n);
  for (size_t i = 0; i < n; i++) {
    uint32_t cp = cp1251 ? cp1251ToUnicode(data[i]) : data[i];
    if (!writer.put(cp)) break;
  }
  return writer.finish();
}

// ---- Parser --------------------------------------------------------------

Parser::Parser() {
  begin(0, nullptr, nullptr);
}

void Parser::begin(uint32_t fileSize, ReadFn read, void *context) {
  _read = read;
  _context = context;
  _fileSize = fileSize;
  memset(&_info, 0, sizeof(_info));
  memset(_rank, 0, sizeof(_rank));
  _coverRank = 0;
  _sourceCount = 0;
  _current = 0;
  _started = false;
  _version = 0;
  _tagUnsync = false;
  _cursor = 0;
  _end = 0;
  _visited = 0;
  _allFramesUnsync = false;
  _pendingFf = false;
}

bool Parser::addId3v2(uint32_t offset, uint32_t limit) {
  if (_sourceCount >= MaxSources || offset > _fileSize || limit < 10) return false;
  if (limit > _fileSize - offset) limit = _fileSize - offset;
  _sources[_sourceCount++] = {SourceKind::Id3v2, offset, limit};
  return true;
}

bool Parser::addRiffInfo(uint32_t offset, uint32_t size) {
  if (_sourceCount >= MaxSources || offset > _fileSize || size < 4) return false;
  if (size > _fileSize - offset) size = _fileSize - offset;
  _sources[_sourceCount++] = {SourceKind::RiffInfo, offset, size};
  return true;
}

bool Parser::addId3v1() {
  if (_sourceCount >= MaxSources || _fileSize < 128) return false;
  _sources[_sourceCount++] = {SourceKind::Id3v1, _fileSize - 128, 128};
  return true;
}

bool Parser::readAt(uint32_t offset, uint8_t *buffer, size_t length) {
  if (!_read || offset > _fileSize || length > _fileSize - offset) return false;
  if (length == 0) return true;
  return _read(_context, offset, buffer, length);
}

void Parser::finishSource() {
  _current++;
  _started = false;
  _visited = 0;
  _tagUnsync = false;
  _allFramesUnsync = false;
  _pendingFf = false;
}

bool Parser::step(uint16_t budget) {
  while (budget > 0 && !done()) {
    budget--;
    const Source &source = _sources[_current];
    switch (source.kind) {
      case SourceKind::Id3v2:
        if (!_started) {
          if (!stepId3v2Start()) finishSource();
        } else if (!stepId3v2Frame()) {
          finishSource();
        }
        break;
      case SourceKind::RiffInfo:
        if (!stepRiffInfo()) finishSource();
        break;
      case SourceKind::Id3v1:
        readId3v1();
        finishSource();
        break;
    }
  }
  return done();
}

// Reads tag bytes at the cursor, removing tag-level unsynchronisation when
// it applies. Never reads past the tag end.
bool Parser::readLogical(uint8_t *buffer, size_t length) {
  if (!_tagUnsync) {
    if (_cursor > _end || length > _end - _cursor) return false;
    if (!readAt(_cursor, buffer, length)) return false;
    _cursor += length;
    return true;
  }
  size_t produced = 0;
  while (produced < length) {
    size_t chunk = length - produced;
    if (chunk > 64) chunk = 64;
    if (_cursor >= _end) return false;
    if (chunk > _end - _cursor) chunk = _end - _cursor;
    uint8_t raw[64];
    if (!readAt(_cursor, raw, chunk)) return false;
    _cursor += chunk;
    for (size_t i = 0; i < chunk; i++) {
      uint8_t b = raw[i];
      if (_pendingFf && b == 0x00) {
        _pendingFf = false;
        continue;
      }
      buffer[produced++] = b;
      _pendingFf = b == 0xff;
    }
  }
  return true;
}

bool Parser::skipLogical(uint32_t length) {
  if (!_tagUnsync) {
    if (_cursor > _end || length > _end - _cursor) return false;
    _cursor += length;
    return true;
  }
  if (length > MaxUnsyncSkipBytes) return false;
  while (length > 0) {
    size_t chunk = length > BufferBytes ? BufferBytes : length;
    if (!readLogical(_buffer, chunk)) return false;
    length -= chunk;
  }
  return true;
}

bool Parser::stepId3v2Start() {
  const Source &source = _sources[_current];
  uint8_t header[10];
  if (!readAt(source.offset, header, sizeof(header))) return false;
  if (memcmp(header, "ID3", 3) != 0) return false;
  _version = header[3];
  if (_version < 2 || _version > 4 || header[4] == 0xff) return false;
  uint8_t flags = header[5];
  uint32_t size = 0;
  if (!readSynchsafe(header + 6, size)) return false;
  if (_version == 2 && (flags & 0x40)) return false;  // v2.2 compression: no scheme defined

  _cursor = source.offset + 10;
  uint32_t available = source.size - 10;
  _end = _cursor + (size < available ? size : available);
  _tagUnsync = _version < 4 && (flags & 0x80) != 0;
  _pendingFf = false;
  _visited = 0;

  if (_version >= 3 && (flags & 0x40)) {
    uint8_t extended[4];
    if (!readLogical(extended, sizeof(extended))) return false;
    if (_version == 3) {
      if (!skipLogical(readBe32(extended))) return false;
    } else {
      uint32_t extendedSize = 0;
      if (!readSynchsafe(extended, extendedSize) || extendedSize < 6 ||
          !skipLogical(extendedSize - 4)) {
        return false;
      }
    }
  }
  // v2.4 applies tag-level unsynchronisation to each frame body instead.
  _allFramesUnsync = _version == 4 && (flags & 0x80) != 0;
  _started = true;
  return true;
}

bool Parser::stepId3v2Frame() {
  const uint8_t version = _version;
  const bool v22 = version == 2;
  const size_t headerBytes = v22 ? 6 : 10;
  if (_visited >= MaxFramesPerSource) return false;
  if (_cursor >= _end || _end - _cursor < headerBytes) return false;

  uint8_t header[10];
  if (!readLogical(header, headerBytes)) return false;
  if (header[0] == 0) return false;  // padding
  const size_t idBytes = v22 ? 3 : 4;
  for (size_t i = 0; i < idBytes; i++) {
    if (!isFrameIdChar(header[i])) return false;
  }
  char id[5] = {0, 0, 0, 0, 0};
  memcpy(id, header, idBytes);

  uint32_t size;
  uint8_t formatFlags = 0;
  if (v22) {
    size = ((uint32_t)header[3] << 16) | ((uint32_t)header[4] << 8) | header[5];
  } else if (version == 3) {
    size = readBe32(header + 4);
    formatFlags = header[9];
  } else {
    if (!readSynchsafe(header + 4, size)) return false;
    formatFlags = header[9];
  }
  _visited++;
  if (size == 0) return true;
  // In unsynchronised tags the remaining raw span is only an upper bound.
  if (size > _end - _cursor) return false;

  bool compressed = false;
  bool encrypted = false;
  bool frameUnsync = false;
  uint32_t prefix = 0;
  if (version == 3) {
    compressed = (formatFlags & 0x80) != 0;
    encrypted = (formatFlags & 0x40) != 0;
    if (formatFlags & 0x20) prefix += 1;  // grouping identity
  } else if (version == 4) {
    if (formatFlags & 0x40) prefix += 1;  // grouping identity
    compressed = (formatFlags & 0x08) != 0;
    encrypted = (formatFlags & 0x04) != 0;
    frameUnsync = _allFramesUnsync || (formatFlags & 0x02) != 0;
    if (formatFlags & 0x01) prefix += 4;  // data length indicator
  }

  const bool picture = v22 ? strcmp(id, "PIC") == 0 : strcmp(id, "APIC") == 0;
  const bool text = id[0] == 'T';
  if (compressed || encrypted || prefix >= size || (!picture && !text)) {
    return skipLogical(size);
  }

  const uint32_t bodyStart = _cursor;
  size_t want = size > MaxTextFrameBytes ? MaxTextFrameBytes : size;
  if (!readLogical(_buffer, want)) return false;
  if (want < size && !skipLogical(size - want)) return false;

  size_t length = want;
  if (frameUnsync) length = deunsync(_buffer, length);
  if (prefix > length) return true;
  const bool rawBody = !_tagUnsync && !frameUnsync;
  handleId3Frame(id, _buffer + prefix, length - prefix, bodyStart + prefix, size - prefix, rawBody);
  return true;
}

void Parser::handleId3Frame(const char *id, const uint8_t *body, size_t length,
                            uint32_t bodyOffset, uint32_t frameSize, bool rawBody) {
  const uint8_t base = (uint8_t)((MaxSources - _current) * 4);
  const bool v22 = _version == 2;
  if (v22 ? strcmp(id, "PIC") == 0 : strcmp(id, "APIC") == 0) {
    handlePicture(body, length, bodyOffset, frameSize, rawBody, v22);
    return;
  }
  if (length < 1) return;
  const uint8_t encoding = body[0];
  if (encoding > 3) return;
  const uint8_t *data = body + 1;
  const size_t dataLength = length - 1;

  struct Mapping {
    const char *v23;
    const char *v22;
    uint8_t field;
    uint8_t rank;
  };
  static const Mapping mappings[] = {
    {"TIT2", "TT2", TitleField, 2},
    {"TPE1", "TP1", ArtistField, 2},
    {"TPE2", "TP2", ArtistField, 1},
    {"TALB", "TAL", AlbumField, 2},
    {"TDRC", "TYE", YearField, 2},
    {"TYER", "TYE", YearField, 2},
    {"TDOR", "TOR", YearField, 1},
    {"TORY", "TOR", YearField, 1},
  };
  for (const Mapping &mapping : mappings) {
    if (strcmp(id, v22 ? mapping.v22 : mapping.v23) != 0) continue;
    if (mapping.field == YearField) {
      setYear(base + mapping.rank, data, dataLength, encoding);
    } else {
      setText(mapping.field, base + mapping.rank, data, dataLength, encoding);
    }
    return;
  }
}

void Parser::handlePicture(const uint8_t *body, size_t length, uint32_t bodyOffset,
                           uint32_t frameSize, bool rawBody, bool v22) {
  if (!rawBody || length < 4) return;
  const uint8_t encoding = body[0];
  if (encoding > 3) return;
  size_t i = 1;
  if (v22) {
    i += 3;  // image format, e.g. "JPG"
  } else {
    while (i < length && body[i] != 0) i++;
    if (i >= length) return;
    i++;  // MIME terminator
  }
  if (i >= length) return;
  const uint8_t type = body[i++];
  // Description, terminated by one NUL (Latin-1/UTF-8) or an aligned NUL pair.
  if (encoding == 1 || encoding == 2) {
    while (i + 1 < length && (body[i] != 0 || body[i + 1] != 0)) i += 2;
    if (i + 1 >= length) return;
    i += 2;
  } else {
    while (i < length && body[i] != 0) i++;
    if (i >= length) return;
    i++;
  }
  if (i >= frameSize) return;

  const uint8_t base = (uint8_t)((MaxSources - _current) * 4);
  const uint8_t rank = base + (type == 3 ? 2 : 1);
  if (rank <= _coverRank) return;
  _coverRank = rank;
  _info.coverOffset = bodyOffset + (uint32_t)i;
  _info.coverLength = frameSize - (uint32_t)i;
  _info.coverType = type;
}

void Parser::setText(uint8_t field, uint8_t rank, const uint8_t *data, size_t length,
                     uint8_t encoding) {
  if (rank <= _rank[field]) return;
  char *target = field == TitleField ? _info.title : (field == ArtistField ? _info.artist : _info.album);
  char decoded[FieldBytes] = {};
  if (decodeText(data, length, encoding, decoded, sizeof(decoded)) == 0) return;
  memcpy(target, decoded, sizeof(decoded));
  _rank[field] = rank;
}

void Parser::setYear(uint8_t rank, const uint8_t *data, size_t length, uint8_t encoding) {
  if (rank <= _rank[YearField]) return;
  char decoded[32];
  decodeText(data, length, encoding, decoded, sizeof(decoded));
  for (size_t i = 0; decoded[i]; i++) {
    if (decoded[i] < '0' || decoded[i] > '9') continue;
    size_t digits = 0;
    while (decoded[i + digits] >= '0' && decoded[i + digits] <= '9') digits++;
    if (digits < 4) return;
    uint16_t year = (uint16_t)((decoded[i] - '0') * 1000 + (decoded[i + 1] - '0') * 100 +
                               (decoded[i + 2] - '0') * 10 + (decoded[i + 3] - '0'));
    if (year < 1000 || year > 2999) return;
    _info.year = year;
    _rank[YearField] = rank;
    return;
  }
}

bool Parser::stepRiffInfo() {
  const Source &source = _sources[_current];
  if (!_started) {
    uint8_t type[4];
    if (!readAt(source.offset, type, sizeof(type)) || memcmp(type, "INFO", 4) != 0) return false;
    _cursor = source.offset + 4;
    _end = source.offset + source.size;
    _visited = 0;
    _started = true;
    return true;
  }
  if (_visited >= MaxFramesPerSource) return false;
  if (_cursor >= _end || _end - _cursor < 8) return false;
  uint8_t header[8];
  if (!readAt(_cursor, header, sizeof(header))) return false;
  _visited++;
  const uint32_t size = readLe32(header + 4);
  const uint32_t payload = _cursor + 8;
  if (size > _end - payload) return false;

  const uint8_t base = (uint8_t)((MaxSources - _current) * 4);
  int8_t field = -1;
  if (memcmp(header, "INAM", 4) == 0) field = TitleField;
  else if (memcmp(header, "IART", 4) == 0) field = ArtistField;
  else if (memcmp(header, "IPRD", 4) == 0) field = AlbumField;
  else if (memcmp(header, "ICRD", 4) == 0) field = YearField;
  if (field >= 0 && size > 0) {
    size_t want = size > MaxTextFrameBytes ? MaxTextFrameBytes : size;
    if (!readAt(payload, _buffer, want)) return false;
    if (field == YearField) setYear(base + 2, _buffer, want, 0);
    else setText((uint8_t)field, base + 2, _buffer, want, 0);
  }

  const uint32_t padded = size + (size & 1U);
  if (padded > _end - payload) return false;
  _cursor = payload + padded;
  return true;
}

void Parser::readId3v1() {
  const Source &source = _sources[_current];
  uint8_t tag[128];
  if (!readAt(source.offset, tag, sizeof(tag)) || memcmp(tag, "TAG", 3) != 0) return;
  const uint8_t base = (uint8_t)((MaxSources - _current) * 4);
  setText(TitleField, base + 2, tag + 3, 30, 0);
  setText(ArtistField, base + 2, tag + 33, 30, 0);
  setText(AlbumField, base + 2, tag + 63, 30, 0);
  setYear(base + 2, tag + 93, 4, 0);
}

}  // namespace AudioTags
