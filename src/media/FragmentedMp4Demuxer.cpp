#include "FragmentedMp4Demuxer.h"

#include <esp32-hal-psram.h>
#include <stdlib.h>
#include <string.h>

namespace {
constexpr uint32_t FourCc(char a, char b, char c, char d) {
  return ((uint32_t)(uint8_t)a << 24) | ((uint32_t)(uint8_t)b << 16) |
         ((uint32_t)(uint8_t)c << 8) | (uint8_t)d;
}

constexpr uint32_t BoxMoov = FourCc('m', 'o', 'o', 'v');
constexpr uint32_t BoxMvex = FourCc('m', 'v', 'e', 'x');
constexpr uint32_t BoxTrex = FourCc('t', 'r', 'e', 'x');
constexpr uint32_t BoxMoof = FourCc('m', 'o', 'o', 'f');
constexpr uint32_t BoxTraf = FourCc('t', 'r', 'a', 'f');
constexpr uint32_t BoxTfhd = FourCc('t', 'f', 'h', 'd');
constexpr uint32_t BoxTfdt = FourCc('t', 'f', 'd', 't');
constexpr uint32_t BoxTrun = FourCc('t', 'r', 'u', 'n');
constexpr uint32_t BoxMdat = FourCc('m', 'd', 'a', 't');
constexpr uint32_t BoxSenc = FourCc('s', 'e', 'n', 'c');
constexpr uint32_t BoxSaiz = FourCc('s', 'a', 'i', 'z');
constexpr uint32_t BoxSaio = FourCc('s', 'a', 'i', 'o');

constexpr uint32_t TfhdBaseDataOffset = 0x000001;
constexpr uint32_t TfhdSampleDescriptionIndex = 0x000002;
constexpr uint32_t TfhdDefaultDuration = 0x000008;
constexpr uint32_t TfhdDefaultSize = 0x000010;
constexpr uint32_t TfhdDefaultFlags = 0x000020;
constexpr uint32_t TfhdDefaultBaseIsMoof = 0x020000;
constexpr uint32_t TrunDataOffset = 0x000001;
constexpr uint32_t TrunFirstSampleFlags = 0x000004;
constexpr uint32_t TrunSampleDuration = 0x000100;
constexpr uint32_t TrunSampleSize = 0x000200;
constexpr uint32_t TrunSampleFlags = 0x000400;
constexpr uint32_t TrunSampleCompositionOffset = 0x000800;

uint32_t bigEndian32(const uint8_t *bytes) {
  return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
         ((uint32_t)bytes[2] << 8) | bytes[3];
}

uint64_t bigEndian64(const uint8_t *bytes) {
  return ((uint64_t)bigEndian32(bytes) << 32) | bigEndian32(bytes + 4);
}
}

FragmentedMp4Demuxer::FragmentedMp4Demuxer()
    : _read(nullptr), _context(nullptr), _sourceSize(0), _firstFragmentOffset(0), _defaults{},
      _defaultCount(0), _hasFragments(false), _error() {}

FragmentedMp4Demuxer::~FragmentedMp4Demuxer() = default;

bool FragmentedMp4Demuxer::open(ReadCallback readCallback, void *context, uint32_t sourceSize) {
  _read = readCallback;
  _context = context;
  _sourceSize = sourceSize;
  _firstFragmentOffset = 0;
  _defaultCount = 0;
  _hasFragments = false;
  _error = String();
  if (!_read || _sourceSize < 8) {
    setError(F("Invalid fragmented MP4 source"));
    return false;
  }
  for (uint32_t offset = 0; offset < _sourceSize; ) {
    Box box;
    if (!readBox(offset, _sourceSize, box)) return false;
    if (box.type == BoxMoov && !parseMovieExtends(box)) return false;
    if (box.type == BoxMoof) {
      if (!_hasFragments) _firstFragmentOffset = box.offset;
      _hasFragments = true;
    }
    offset = box.endOffset;
  }
  if (!_hasFragments) setError(F("MP4 has no movie fragments"));
  return _hasFragments;
}

bool FragmentedMp4Demuxer::hasFragments() const { return _hasFragments; }
const String &FragmentedMp4Demuxer::lastError() const { return _error; }

bool FragmentedMp4Demuxer::initCursor(uint32_t trackId, Cursor &cursor) const {
  resetCursor(cursor);
  if (!trackId || !_hasFragments) return false;
  cursor.trackId = trackId;
  cursor.nextTopLevelOffset = _firstFragmentOffset;
  return true;
}

void FragmentedMp4Demuxer::resetCursor(Cursor &cursor) const {
  free(cursor.samples);
  cursor = {};
}

bool FragmentedMp4Demuxer::nextSample(Cursor &cursor, Sample &sample) {
  if (!cursor.trackId || cursor.finished) return false;
  while (cursor.sampleIndex >= cursor.sampleCount) {
    if (!loadNextFragment(cursor)) return false;
  }
  sample = cursor.samples[cursor.sampleIndex++];
  return true;
}

bool FragmentedMp4Demuxer::countSamples(uint32_t trackId, uint32_t &count) {
  count = 0;
  Cursor cursor;
  if (!initCursor(trackId, cursor)) return false;
  Sample sample;
  while (nextSample(cursor, sample)) {
    if (count == 0xffffffffUL) {
      resetCursor(cursor);
      setError(F("Fragment sample count is too large"));
      return false;
    }
    ++count;
  }
  const bool ok = !_error.length();
  resetCursor(cursor);
  return ok;
}

bool FragmentedMp4Demuxer::fillPresentationTimestamps(uint32_t trackId, int64_t *timestamps,
                                                       uint32_t capacity, uint32_t &duration) {
  duration = 0;
  if (!timestamps || !capacity) return false;
  Cursor cursor;
  if (!initCursor(trackId, cursor)) return false;
  Sample sample;
  uint32_t index = 0;
  while (nextSample(cursor, sample)) {
    if (index >= capacity) {
      resetCursor(cursor);
      setError(F("Fragment sample count changed while scanning"));
      return false;
    }
    timestamps[index++] = (int64_t)sample.decodeTimestamp + sample.compositionOffset;
    const uint64_t end = (uint64_t)sample.decodeTimestamp + sample.duration;
    if (end > 0xffffffffULL) {
      resetCursor(cursor);
      setError(F("Fragment timeline exceeds supported range"));
      return false;
    }
    if ((uint32_t)end > duration) duration = (uint32_t)end;
  }
  const bool ok = !_error.length() && index == capacity;
  resetCursor(cursor);
  if (!ok && !_error.length()) setError(F("Incomplete fragmented MP4 timeline"));
  return ok;
}

bool FragmentedMp4Demuxer::read(uint32_t offset, void *destination, uint32_t bytes) const {
  return _read && destination && bytes && offset <= _sourceSize && bytes <= _sourceSize - offset &&
         _read(offset, destination, bytes, _context);
}

bool FragmentedMp4Demuxer::readU32(uint32_t offset, uint32_t &value) const {
  uint8_t bytes[4] = {};
  if (!read(offset, bytes, sizeof(bytes))) return false;
  value = bigEndian32(bytes);
  return true;
}

bool FragmentedMp4Demuxer::readU64(uint32_t offset, uint64_t &value) const {
  uint8_t bytes[8] = {};
  if (!read(offset, bytes, sizeof(bytes))) return false;
  value = bigEndian64(bytes);
  return true;
}

bool FragmentedMp4Demuxer::readBox(uint32_t offset, uint32_t rangeEnd, Box &box) const {
  uint8_t header[16] = {};
  if (offset > rangeEnd || rangeEnd - offset < 8 || !read(offset, header, 8)) {
    const_cast<FragmentedMp4Demuxer *>(this)->setError(F("Truncated MP4 box header"));
    return false;
  }
  uint64_t size = bigEndian32(header);
  uint32_t headerBytes = 8;
  if (size == 1) {
    if (rangeEnd - offset < 16 || !read(offset + 8, header + 8, 8)) {
      const_cast<FragmentedMp4Demuxer *>(this)->setError(F("Truncated 64-bit MP4 box"));
      return false;
    }
    size = bigEndian64(header + 8);
    headerBytes = 16;
  } else if (size == 0) {
    size = rangeEnd - offset;
  }
  if (size < headerBytes || size > (uint64_t)(rangeEnd - offset) ||
      size > 0xffffffffULL || offset + (uint32_t)size < offset) {
    const_cast<FragmentedMp4Demuxer *>(this)->setError(F("Invalid MP4 box bounds"));
    return false;
  }
  box = {offset, offset + headerBytes, offset + (uint32_t)size, bigEndian32(header + 4)};
  return true;
}

bool FragmentedMp4Demuxer::parseMovieExtends(const Box &moov) {
  for (uint32_t offset = moov.payloadOffset; offset < moov.endOffset; ) {
    Box child;
    if (!readBox(offset, moov.endOffset, child)) return false;
    if (child.type == BoxMvex) {
      for (uint32_t nestedOffset = child.payloadOffset; nestedOffset < child.endOffset; ) {
        Box nested;
        if (!readBox(nestedOffset, child.endOffset, nested)) return false;
        if (nested.type == BoxTrex && !parseTrackExtends(nested)) return false;
        nestedOffset = nested.endOffset;
      }
    }
    offset = child.endOffset;
  }
  return true;
}

bool FragmentedMp4Demuxer::parseTrackExtends(const Box &trex) {
  if (trex.endOffset - trex.payloadOffset < 24) {
    setError(F("Truncated fMP4 track defaults"));
    return false;
  }
  uint8_t bytes[24] = {};
  if (!read(trex.payloadOffset, bytes, sizeof(bytes))) {
    setError(F("Unable to read fMP4 track defaults"));
    return false;
  }
  const uint32_t trackId = bigEndian32(bytes + 4);
  if (!trackId) {
    setError(F("Invalid fMP4 track ID"));
    return false;
  }
  for (uint8_t index = 0; index < _defaultCount; ++index) {
    if (_defaults[index].trackId != trackId) continue;
    _defaults[index] = {trackId, bigEndian32(bytes + 8), bigEndian32(bytes + 12),
                        bigEndian32(bytes + 16), bigEndian32(bytes + 20)};
    return true;
  }
  if (_defaultCount >= MaxTrackDefaults) {
    setError(F("Too many fMP4 tracks"));
    return false;
  }
  _defaults[_defaultCount++] = {trackId, bigEndian32(bytes + 8), bigEndian32(bytes + 12),
                                bigEndian32(bytes + 16), bigEndian32(bytes + 20)};
  return true;
}

bool FragmentedMp4Demuxer::loadNextFragment(Cursor &cursor) {
  cursor.sampleCount = cursor.sampleIndex = 0;
  while (cursor.nextTopLevelOffset < _sourceSize) {
    Box box;
    if (!readBox(cursor.nextTopLevelOffset, _sourceSize, box)) return false;
    cursor.nextTopLevelOffset = box.endOffset;
    if (box.type != BoxMoof) continue;
    if (!parseMovieFragment(box, cursor)) return false;
    if (cursor.sampleCount) return true;
  }
  cursor.finished = true;
  return false;
}

bool FragmentedMp4Demuxer::parseMovieFragment(const Box &moof, Cursor &cursor) {
  for (uint32_t offset = moof.payloadOffset; offset < moof.endOffset; ) {
    Box child;
    if (!readBox(offset, moof.endOffset, child)) return false;
    if (child.type == BoxTraf && !parseTrackFragment(moof, child, cursor)) return false;
    offset = child.endOffset;
  }
  return true;
}

bool FragmentedMp4Demuxer::parseTrackFragment(const Box &moof, const Box &traf, Cursor &cursor) {
  Box tfhd, tfdt;
  bool encrypted = false;
  for (uint32_t offset = traf.payloadOffset; offset < traf.endOffset; ) {
    Box child;
    if (!readBox(offset, traf.endOffset, child)) return false;
    if (child.type == BoxTfhd) tfhd = child;
    else if (child.type == BoxTfdt) tfdt = child;
    else if (child.type == BoxSenc || child.type == BoxSaiz || child.type == BoxSaio) encrypted = true;
    offset = child.endOffset;
  }
  if (!tfhd.type) {
    setError(F("fMP4 fragment is missing tfhd"));
    return false;
  }
  Defaults defaults = {};
  uint64_t baseDataOffset = 0;
  bool baseDataOffsetSet = false;
  bool defaultBaseIsMoof = false;
  if (!parseTrackHeader(tfhd, moof.offset, defaults, baseDataOffset, baseDataOffsetSet, defaultBaseIsMoof)) return false;
  if (encrypted) {
    setError(F("Encrypted fragmented MP4 is unsupported"));
    return false;
  }
  if (defaults.trackId != cursor.trackId) return true;
  uint64_t decodeTime = 0;
  if (!tfdt.type) {
    setError(F("fMP4 fragment is missing tfdt"));
    return false;
  }
  if (!parseDecodeTime(tfdt, decodeTime)) return false;
  uint64_t implicitDataOffset = baseDataOffsetSet && baseDataOffset != moof.offset
                                    ? baseDataOffset : firstMediaDataOffset(moof);
  if (!implicitDataOffset) {
    setError(F("fMP4 fragment has no media data"));
    return false;
  }
  for (uint32_t offset = traf.payloadOffset; offset < traf.endOffset; ) {
    Box child;
    if (!readBox(offset, traf.endOffset, child)) return false;
    if (child.type == BoxTrun && !appendRun(moof, child, defaults, baseDataOffset, baseDataOffsetSet,
                                            defaultBaseIsMoof, implicitDataOffset, decodeTime, cursor)) return false;
    offset = child.endOffset;
  }
  return true;
}

bool FragmentedMp4Demuxer::parseTrackHeader(const Box &tfhd, uint32_t moofOffset, Defaults &defaults,
                                             uint64_t &baseDataOffset, bool &baseDataOffsetSet,
                                             bool &defaultBaseIsMoof) {
  if (tfhd.endOffset - tfhd.payloadOffset < 8) {
    setError(F("Truncated fMP4 tfhd"));
    return false;
  }
  uint8_t header[8] = {};
  if (!read(tfhd.payloadOffset, header, sizeof(header))) {
    setError(F("Unable to read fMP4 tfhd"));
    return false;
  }
  const uint32_t flags = bigEndian32(header) & 0x00ffffffUL;
  const uint32_t trackId = bigEndian32(header + 4);
  const Defaults *base = defaultsForTrack(trackId);
  defaults = base ? *base : Defaults{};
  defaults.trackId = trackId;
  uint32_t offset = tfhd.payloadOffset + 8;
  baseDataOffset = moofOffset;
  baseDataOffsetSet = false;
  defaultBaseIsMoof = (flags & TfhdDefaultBaseIsMoof) != 0;
  auto readField = [&](uint32_t &value) -> bool {
    if (offset > tfhd.endOffset || tfhd.endOffset - offset < 4 || !readU32(offset, value)) return false;
    offset += 4;
    return true;
  };
  if (flags & TfhdBaseDataOffset) {
    if (offset > tfhd.endOffset || tfhd.endOffset - offset < 8 || !readU64(offset, baseDataOffset) ||
        baseDataOffset > 0xffffffffULL) {
      setError(F("Unsupported fMP4 base data offset"));
      return false;
    }
    offset += 8;
    baseDataOffsetSet = true;
  }
  uint32_t value = 0;
  if (flags & TfhdSampleDescriptionIndex) {
    if (!readField(value) || value != defaults.sampleDescriptionIndex) {
      setError(F("fMP4 sample-description changes are unsupported"));
      return false;
    }
  }
  if (flags & TfhdDefaultDuration) { if (!readField(defaults.duration)) { setError(F("Truncated fMP4 tfhd")); return false; } }
  if (flags & TfhdDefaultSize) { if (!readField(defaults.size)) { setError(F("Truncated fMP4 tfhd")); return false; } }
  if (flags & TfhdDefaultFlags) { if (!readField(defaults.flags)) { setError(F("Truncated fMP4 tfhd")); return false; } }
  if (!trackId) { setError(F("Invalid fMP4 fragment track ID")); return false; }
  return true;
}

bool FragmentedMp4Demuxer::parseDecodeTime(const Box &tfdt, uint64_t &decodeTime) {
  if (tfdt.endOffset - tfdt.payloadOffset < 8) { setError(F("Truncated fMP4 tfdt")); return false; }
  uint32_t full = 0;
  if (!readU32(tfdt.payloadOffset, full)) { setError(F("Unable to read fMP4 tfdt")); return false; }
  const uint8_t version = full >> 24;
  if (version == 0) {
    uint32_t value = 0;
    if (!readU32(tfdt.payloadOffset + 4, value)) {
      setError(F("Unable to read fMP4 tfdt"));
      return false;
    }
    decodeTime = value;
    return true;
  }
  if (version == 1 && tfdt.endOffset - tfdt.payloadOffset >= 12 && readU64(tfdt.payloadOffset + 4, decodeTime)) return true;
  setError(F("Unsupported fMP4 tfdt"));
  return false;
}

bool FragmentedMp4Demuxer::appendRun(const Box &moof, const Box &trun, const Defaults &defaults,
                                     uint64_t baseDataOffset, bool baseDataOffsetSet, bool defaultBaseIsMoof,
                                     uint64_t &implicitDataOffset, uint64_t &decodeTime, Cursor &cursor) {
  (void)moof;
  (void)baseDataOffsetSet;
  (void)defaultBaseIsMoof;
  if (trun.endOffset - trun.payloadOffset < 8) { setError(F("Truncated fMP4 trun")); return false; }
  uint8_t header[8] = {};
  if (!read(trun.payloadOffset, header, sizeof(header))) { setError(F("Unable to read fMP4 trun")); return false; }
  const uint32_t full = bigEndian32(header);
  const uint8_t version = full >> 24;
  const uint32_t flags = full & 0x00ffffffUL;
  const uint32_t sampleCount = bigEndian32(header + 4);
  uint32_t offset = trun.payloadOffset + 8;
  uint64_t dataOffset = implicitDataOffset;
  if (flags & TrunDataOffset) {
    uint32_t raw = 0;
    if (offset > trun.endOffset || trun.endOffset - offset < 4 || !readU32(offset, raw)) { setError(F("Truncated fMP4 trun offset")); return false; }
    offset += 4;
    const int64_t resolved = (int64_t)baseDataOffset + (int32_t)raw;
    if (resolved < 0 || resolved > 0xffffffffLL) { setError(F("Invalid fMP4 media offset")); return false; }
    dataOffset = (uint64_t)resolved;
  }
  if (flags & TrunFirstSampleFlags) {
    if (offset > trun.endOffset || trun.endOffset - offset < 4) { setError(F("Truncated fMP4 sample flags")); return false; }
    offset += 4;
  }
  if (sampleCount > 0xffffffffUL - cursor.sampleCount || !reserveSamples(cursor, cursor.sampleCount + sampleCount)) return false;
  for (uint32_t index = 0; index < sampleCount; ++index) {
    uint32_t duration = defaults.duration, size = defaults.size, ignored = 0, compositionRaw = 0;
    auto nextField = [&](uint32_t &value) -> bool {
      if (offset > trun.endOffset || trun.endOffset - offset < 4 || !readU32(offset, value)) return false;
      offset += 4;
      return true;
    };
    if ((flags & TrunSampleDuration) && !nextField(duration)) { setError(F("Truncated fMP4 sample duration")); return false; }
    if ((flags & TrunSampleSize) && !nextField(size)) { setError(F("Truncated fMP4 sample size")); return false; }
    if ((flags & TrunSampleFlags) && !nextField(ignored)) { setError(F("Truncated fMP4 sample flags")); return false; }
    if ((flags & TrunSampleCompositionOffset) && !nextField(compositionRaw)) { setError(F("Truncated fMP4 composition offset")); return false; }
    if (!duration || !size || decodeTime > 0xffffffffULL || dataOffset > _sourceSize || size > _sourceSize - dataOffset) {
      setError(F("Invalid fMP4 sample bounds"));
      return false;
    }
    const int64_t compositionOffset = (flags & TrunSampleCompositionOffset) && version == 1
                                          ? (int64_t)(int32_t)compositionRaw : (int64_t)compositionRaw;
    cursor.samples[cursor.sampleCount++] = {(uint32_t)dataOffset, size, (uint32_t)decodeTime,
                                             duration, compositionOffset};
    dataOffset += size;
    decodeTime += duration;
    if (decodeTime > 0xffffffffULL) { setError(F("Fragment timeline exceeds supported range")); return false; }
  }
  implicitDataOffset = dataOffset;
  return true;
}

bool FragmentedMp4Demuxer::reserveSamples(Cursor &cursor, uint32_t count) {
  if (count <= cursor.sampleCapacity) return true;
  if ((uint64_t)count * sizeof(Sample) > 0xffffffffULL) {
    setError(F("Fragment sample metadata is too large"));
    return false;
  }
  Sample *replacement = static_cast<Sample *>(ps_malloc((size_t)count * sizeof(Sample)));
  if (!replacement) {
    setError(F("Fragment sample metadata needs PSRAM"));
    return false;
  }
  if (cursor.samples && cursor.sampleCount) memcpy(replacement, cursor.samples, cursor.sampleCount * sizeof(Sample));
  free(cursor.samples);
  cursor.samples = replacement;
  cursor.sampleCapacity = count;
  return true;
}

uint64_t FragmentedMp4Demuxer::firstMediaDataOffset(const Box &moof) const {
  uint32_t offset = moof.endOffset;
  while (offset < _sourceSize) {
    Box box;
    if (!readBox(offset, _sourceSize, box)) return 0;
    if (box.type == BoxMdat) return box.payloadOffset;
    if (box.type == BoxMoof) return 0;
    offset = box.endOffset;
  }
  return 0;
}

const FragmentedMp4Demuxer::Defaults *FragmentedMp4Demuxer::defaultsForTrack(uint32_t trackId) const {
  for (uint8_t index = 0; index < _defaultCount; ++index)
    if (_defaults[index].trackId == trackId) return &_defaults[index];
  return nullptr;
}

void FragmentedMp4Demuxer::setError(const __FlashStringHelper *error) { _error = error; }
void FragmentedMp4Demuxer::setError(const String &error) { _error = error; }
