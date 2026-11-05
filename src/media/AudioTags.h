#pragma once

#include <stddef.h>
#include <stdint.h>

// Arduino-free reader for the descriptive tags of a local audio file: ID3v2.2,
// ID3v2.3, and ID3v2.4 (at the start of an MP3 or in a WAV `id3 ` chunk),
// ID3v1/v1.1, and RIFF LIST/INFO. It yields bounded UTF-8 text plus the file
// position of an embedded picture; it never reads the picture bytes. The
// caller classifies a picture through FileFormats before decoding it.
//
// Work is incremental: step() visits at most a caller-chosen number of
// frames or chunks, so a UI task can spread a large tag over several ticks.
// Every size is checked against its enclosing tag or chunk and the file; the
// first inconsistent size ends that source without failing the others.
// tests/audio_tags covers the contract on the host.
namespace AudioTags {

constexpr size_t FieldBytes = 128;       // including the terminating NUL
constexpr uint16_t MaxFramesPerSource = 256;
constexpr size_t MaxTextFrameBytes = 256;

// Reads exactly `length` bytes at absolute file `offset`; false on any error.
typedef bool (*ReadFn)(void *context, uint32_t offset, uint8_t *buffer, size_t length);

struct Info {
  char title[FieldBytes];
  char artist[FieldBytes];
  char album[FieldBytes];
  uint16_t year;          // 0 when unknown; otherwise 1000–2999
  uint32_t coverOffset;   // absolute offset of the raw picture bytes
  uint32_t coverLength;   // 0 when there is no usable picture
  uint8_t coverType;      // ID3 picture type; 3 is the front cover

  bool hasCover() const { return coverLength > 0; }
};

class Parser {
public:
  Parser();

  // Starts over with an empty Info for a file of `fileSize` bytes.
  void begin(uint32_t fileSize, ReadFn read, void *context);

  // Queue sources in priority order; earlier sources win for each field.
  // An ID3v2 tag starting at `offset` and confined to `limit` bytes.
  bool addId3v2(uint32_t offset, uint32_t limit);
  // The body of a RIFF `LIST` chunk (starting with its "INFO" type).
  bool addRiffInfo(uint32_t offset, uint32_t size);
  // An ID3v1 tag in the file's last 128 bytes, if one is present.
  bool addId3v1();

  // Advances through at most `budget` frames or chunks. Returns true once
  // every queued source has been read (or abandoned).
  bool step(uint16_t budget);
  bool done() const { return _current >= _sourceCount; }

  const Info &info() const { return _info; }

private:
  enum class SourceKind : uint8_t { Id3v2, RiffInfo, Id3v1 };
  struct Source {
    SourceKind kind;
    uint32_t offset;
    uint32_t size;
  };

  static constexpr uint8_t MaxSources = 4;
  static constexpr uint16_t BufferBytes = MaxTextFrameBytes + 16;

  ReadFn _read;
  void *_context;
  uint32_t _fileSize;
  Info _info;
  uint8_t _rank[4];  // title, artist, album, year
  uint8_t _coverRank;
  Source _sources[MaxSources];
  uint8_t _sourceCount;
  uint8_t _current;

  // State of the ID3v2 or INFO source being walked.
  bool _started;
  uint8_t _version;       // ID3v2 major version
  bool _tagUnsync;        // v2.2/v2.3 tag-level unsynchronisation
  bool _allFramesUnsync;  // v2.4 tag flag: every frame body is unsynchronised
  uint32_t _cursor;       // absolute offset of the next frame/chunk
  uint32_t _end;          // absolute end of frames/chunks
  uint16_t _visited;
  bool _pendingFf;        // de-unsync state: the last raw byte was 0xFF

  uint8_t _buffer[BufferBytes];

  bool readAt(uint32_t offset, uint8_t *buffer, size_t length);
  void finishSource();
  bool stepId3v2Start();
  bool stepId3v2Frame();
  bool stepRiffInfo();
  void readId3v1();
  bool readLogical(uint8_t *buffer, size_t length);
  bool skipLogical(uint32_t length);

  void handleId3Frame(const char *id, const uint8_t *body, size_t length,
                      uint32_t bodyOffset, uint32_t frameSize, bool rawBody);
  void handlePicture(const uint8_t *body, size_t length, uint32_t bodyOffset,
                     uint32_t frameSize, bool rawBody, bool v22);
  void setText(uint8_t field, uint8_t rank, const uint8_t *data, size_t length, uint8_t encoding);
  void setYear(uint8_t rank, const uint8_t *data, size_t length, uint8_t encoding);
};

// Exposed for tests: converts tag text to bounded, trimmed UTF-8.
// Encoding: 0 Latin-1/Windows-1251/UTF-8 guess, 1 UTF-16 with BOM,
// 2 UTF-16BE, 3 UTF-8. Stops at the first NUL terminator.
size_t decodeText(const uint8_t *data, size_t length, uint8_t encoding,
                  char *out, size_t outBytes);

}  // namespace AudioTags
