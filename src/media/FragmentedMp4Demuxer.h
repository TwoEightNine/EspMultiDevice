#pragma once

#include <Arduino.h>

// A deliberately narrow, project-owned reader for complete local fragmented
// MP4 files. It consumes only fragment metadata and leaves compressed packets
// in the source file, so a long recording does not need a full sample index.
class FragmentedMp4Demuxer {
public:
  typedef bool (*ReadCallback)(uint32_t offset, void *destination, uint32_t bytes, void *context);

  struct Sample {
    uint32_t offset = 0;
    uint32_t bytes = 0;
    uint32_t decodeTimestamp = 0;
    uint32_t duration = 0;
    int64_t compositionOffset = 0;
  };

  struct Cursor {
    uint32_t trackId = 0;
    uint32_t nextTopLevelOffset = 0;
    Sample *samples = nullptr;
    uint32_t sampleCount = 0;
    uint32_t sampleIndex = 0;
    uint32_t sampleCapacity = 0;
    bool finished = false;
  };

  FragmentedMp4Demuxer();
  ~FragmentedMp4Demuxer();

  bool open(ReadCallback read, void *context, uint32_t sourceSize);
  bool hasFragments() const;
  const String &lastError() const;

  bool initCursor(uint32_t trackId, Cursor &cursor) const;
  bool nextSample(Cursor &cursor, Sample &sample);
  void resetCursor(Cursor &cursor) const;

  bool countSamples(uint32_t trackId, uint32_t &count);
  bool fillPresentationTimestamps(uint32_t trackId, int64_t *timestamps, uint32_t capacity,
                                  uint32_t &duration);

private:
  struct Box {
    uint32_t offset = 0;
    uint32_t payloadOffset = 0;
    uint32_t endOffset = 0;
    uint32_t type = 0;
  };
  struct Defaults {
    uint32_t trackId = 0;
    uint32_t sampleDescriptionIndex = 0;
    uint32_t duration = 0;
    uint32_t size = 0;
    uint32_t flags = 0;
  };

  static constexpr uint8_t MaxTrackDefaults = 8;

  ReadCallback _read;
  void *_context;
  uint32_t _sourceSize;
  uint32_t _firstFragmentOffset;
  Defaults _defaults[MaxTrackDefaults];
  uint8_t _defaultCount;
  bool _hasFragments;
  String _error;

  bool read(uint32_t offset, void *destination, uint32_t bytes) const;
  bool readU32(uint32_t offset, uint32_t &value) const;
  bool readU64(uint32_t offset, uint64_t &value) const;
  bool readBox(uint32_t offset, uint32_t rangeEnd, Box &box) const;
  bool parseMovieExtends(const Box &moov);
  bool parseTrackExtends(const Box &trex);
  bool loadNextFragment(Cursor &cursor);
  bool parseMovieFragment(const Box &moof, Cursor &cursor);
  bool parseTrackFragment(const Box &moof, const Box &traf, Cursor &cursor);
  bool parseTrackHeader(const Box &tfhd, uint32_t moofOffset, Defaults &defaults,
                        uint64_t &baseDataOffset, bool &baseDataOffsetSet, bool &defaultBaseIsMoof);
  bool parseDecodeTime(const Box &tfdt, uint64_t &decodeTime);
  bool appendRun(const Box &moof, const Box &trun, const Defaults &defaults, uint64_t baseDataOffset,
                 bool baseDataOffsetSet, bool defaultBaseIsMoof, uint64_t &implicitDataOffset,
                 uint64_t &decodeTime, Cursor &cursor);
  bool reserveSamples(Cursor &cursor, uint32_t count);
  uint64_t firstMediaDataOffset(const Box &moof) const;
  const Defaults *defaultsForTrack(uint32_t trackId) const;
  void setError(const __FlashStringHelper *error);
  void setError(const String &error);
};
