#define MINIMP3_ONLY_MP3
#define MINIMP3_IMPLEMENTATION
#include "../../../third_party/minimp3/minimp3.h"

#include "AudioSource.h"

#include "../../core/SpiBusLock.h"
#include "../../media/FileFormats.h"
#include "../../services/audio/SpeakerController.h"

#include <SD.h>
#include <esp_heap_caps.h>
#include <string.h>

namespace {
uint16_t readLe16(const uint8_t *buffer) {
  return (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8);
}

uint32_t readLe32(const uint8_t *buffer) {
  return (uint32_t)buffer[0]
    | ((uint32_t)buffer[1] << 8)
    | ((uint32_t)buffer[2] << 16)
    | ((uint32_t)buffer[3] << 24);
}

void writeLe16(uint8_t *buffer, uint16_t value) {
  buffer[0] = (uint8_t)value;
  buffer[1] = (uint8_t)(value >> 8);
}

void writeLe32(uint8_t *buffer, uint32_t value) {
  buffer[0] = (uint8_t)value;
  buffer[1] = (uint8_t)(value >> 8);
  buffer[2] = (uint8_t)(value >> 16);
  buffer[3] = (uint8_t)(value >> 24);
}

int32_t clamp16(int64_t value) {
  if (value < -32768) return -32768;
  if (value > 32767) return 32767;
  return (int32_t)value;
}

bool isPcmSubFormat(const uint8_t *guid) {
  static const uint8_t pcmGuid[16] = {
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71
  };
  return memcmp(guid, pcmGuid, sizeof(pcmGuid)) == 0;
}

uint32_t readBe32(const uint8_t *buffer) {
  return ((uint32_t)buffer[0] << 24)
    | ((uint32_t)buffer[1] << 16)
    | ((uint32_t)buffer[2] << 8)
    | (uint32_t)buffer[3];
}

uint32_t readSynchsafe32(const uint8_t *buffer) {
  return ((uint32_t)(buffer[0] & 0x7f) << 21)
    | ((uint32_t)(buffer[1] & 0x7f) << 14)
    | ((uint32_t)(buffer[2] & 0x7f) << 7)
    | (uint32_t)(buffer[3] & 0x7f);
}

const uint16_t Mpeg1Layer3Bitrates[] = {
  0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320
};

const uint16_t Mpeg2Layer3Bitrates[] = {
  0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160
};
}

AudioSource::AudioSource(SecretStorage &secretStorage, MediaCache &mediaCache)
  : _secretStorage(secretStorage),
    _mediaCache(mediaCache),
    _secretAccess(false),
    _format(Format::None),
    _streamReady(false),
    _busLocked(false),
    _mp3CachePending(false),
    _dataOffset(0),
    _dataBytes(0),
    _sourceFileSize(0),
    _sourceWriteTime(0),
    _frameCount(0),
    _framePosition(0),
    _sampleRate(0),
    _channels(0),
    _bitsPerSample(0),
    _blockAlign(0),
    _tagsParsing(false),
    _wavInfoOffset(0),
    _wavInfoSize(0),
    _wavId3Offset(0),
    _wavId3Size(0),
    _mp3Analyzing(false),
    _mp3DataOffset(0),
    _mp3AudioEnd(0),
    _mp3AnalysisPosition(0),
    _mp3InputOffset(0),
    _mp3InputBytes(0),
    _mp3PcmFrames(0),
    _mp3PcmPosition(0),
    _tocStorage(nullptr),
    _tocReady(false),
    _mp3FrameSamples(0),
    _mp3ScanFrames(0),
    _mp3SkipFrames(0),
    _mp3SkipSamples(0) {
  mp3dec_init(&_mp3Decoder);
}

bool AudioSource::lockBus(uint32_t waitMs) {
  if (_busLocked) return true;
  if (!SpiBusLock::lock(pdMS_TO_TICKS(waitMs))) return false;
  _busLocked = true;
  return true;
}

void AudioSource::unlockBus() {
  if (!_busLocked) return;
  _busLocked = false;
  SpiBusLock::unlock();
}

void AudioSource::closeFile() {
  if (_file) {
    const bool took = !_busLocked && lockBus(BusOpenWaitMs);
    // Close even if the bus stayed busy, so the handle cannot leak; closing a
    // read-only file does not write to the card.
    _file.close();
    if (took) unlockBus();
  }
  _mp3CachePending = false;
  _tagsParsing = false;
  _mp3Analyzing = false;
  _streamReady = false;
}

void AudioSource::reset() {
  closeFile();
  _path = "";
  _secretAccess = false;
  _error = "";
  _format = Format::None;
  _dataOffset = 0;
  _dataBytes = 0;
  _sourceFileSize = 0;
  _sourceWriteTime = 0;
  _frameCount = 0;
  _framePosition = 0;
  _sampleRate = 0;
  _channels = 0;
  _bitsPerSample = 0;
  _blockAlign = 0;
  _tags.begin(0, nullptr, nullptr);
  _wavInfoOffset = 0;
  _wavInfoSize = 0;
  _wavId3Offset = 0;
  _wavId3Size = 0;
  _mp3DataOffset = 0;
  _mp3AudioEnd = 0;
  _mp3AnalysisPosition = 0;
  _mp3InputOffset = 0;
  _mp3InputBytes = 0;
  _mp3PcmFrames = 0;
  _mp3PcmPosition = 0;
  _mp3FrameSamples = 0;
  _mp3ScanFrames = 0;
  _mp3SkipFrames = 0;
  _mp3SkipSamples = 0;
  releaseSeekTable();
  mp3dec_init(&_mp3Decoder);
}

bool AudioSource::allocateSeekTable() {
  if (!_tocStorage) {
    _tocStorage = static_cast<uint32_t *>(heap_caps_malloc(
      sizeof(uint32_t) * Mp3SeekTable::MaxEntries, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (!_tocStorage) {
    // Playback still works; the MP3 just cannot seek.
    Serial.println(F("MP3 seek table: no PSRAM"));
    _toc.detach();
    return false;
  }
  _toc.attach(_tocStorage, Mp3SeekTable::MaxEntries);
  return true;
}

void AudioSource::releaseSeekTable() {
  _toc.detach();
  _tocReady = false;
  heap_caps_free(_tocStorage);
  _tocStorage = nullptr;
}

bool AudioSource::open(const String &path, bool secretAccess) {
  reset();
  _path = path;
  _secretAccess = secretAccess;
  switch (FileFormats::fromPath(path)) {
    case FileFormats::Format::Wav: _format = Format::Wav; break;
    case FileFormats::Format::Mp3: _format = Format::Mp3; break;
    default: return fail(F("Unsupported audio file"));
  }
  if (!lockBus(BusOpenWaitMs)) return fail(F("SD card busy"));
  _file = _secretStorage.open(path, FILE_READ, _secretAccess);
  unlockBus();
  if (!_file) return fail(F("Unable to open audio file"));
  SourceFingerprint fingerprint{};
  if (_mediaCache.captureFingerprint(_file, fingerprint) != MediaCache::Result::Complete) {
    return fail(F("Unable to inspect audio file"));
  }
  _sourceFileSize = fingerprint.size;
  _sourceWriteTime = fingerprint.modified;
  if (_format == Format::Wav) {
    if (!lockBus(BusOpenWaitMs)) return fail(F("SD card busy"));
    const bool parsed = parseWav();
    unlockBus();
    if (!parsed) return false;
  }
  if (_format == Format::Mp3 && !beginMp3Analysis()) return false;
  beginTags();
  return true;
}

void AudioSource::loadStep() {
  if (!_file) {
    _tagsParsing = false;
    _mp3Analyzing = false;
    return;
  }
  if (!loading() || !lockBus(BusWaitMs)) return;
  _streamReady = false;
  if (_tagsParsing) {
    if (_tags.step(TagFramesPerStep)) {
      _tagsParsing = false;
      const AudioTags::Info &info = _tags.info();
      if (info.title[0] || info.artist[0] || info.album[0] || info.year || info.hasCover()) {
        Serial.printf("Audio tags: title=\"%s\" artist=\"%s\" album=\"%s\" year=%u cover=%lu@%lu\n",
                      info.title, info.artist, info.album, (unsigned)info.year,
                      (unsigned long)info.coverLength, (unsigned long)info.coverOffset);
      }
    }
  } else {
    analyzeMp3Step();
  }
  unlockBus();
  if (_mp3CachePending) {
    _mp3CachePending = false;
    writeMp3Cache();
  }
}

uint32_t AudioSource::bitrateKbps() const {
  if (_format == Format::Wav) return (uint32_t)(((uint64_t)_sampleRate * _blockAlign * 8) / 1000);
  if (_format != Format::Mp3 || _frameCount == 0 || _sampleRate == 0 || loading()) return 0;
  const uint64_t bits = (uint64_t)(_mp3AudioEnd - _mp3DataOffset) * 8;
  return (uint32_t)((bits * _sampleRate / _frameCount + 500) / 1000);
}

bool AudioSource::canSeek() const {
  return _file && !loading() && _frameCount > 0 &&
         (_format == Format::Wav || (_format == Format::Mp3 && _tocReady));
}

bool AudioSource::seek(uint32_t frame) {
  if (!_file || loading()) return false;
  if (frame > _frameCount) frame = _frameCount;
  if (_format == Format::Mp3 && frame != 0 && !_tocReady) return false;
  _framePosition = frame;
  _mp3PcmFrames = 0;
  _mp3PcmPosition = 0;
  _mp3SkipFrames = 0;
  _mp3SkipSamples = 0;
  _streamReady = false;
  return true;
}

AudioSource::StreamResult AudioSource::stream(SpeakerController &speaker) {
  if (!_file || failed()) return StreamResult::Failed;
  if (ended()) return StreamResult::Ended;
  if (!lockBus(BusWaitMs)) return StreamResult::Busy;
  if (!_streamReady && !prepareStream()) {
    unlockBus();
    return StreamResult::Failed;
  }
  if (_format == Format::Mp3 && _mp3SkipFrames > 0) {
    const StreamResult skipped = discardMp3Frames();
    unlockBus();
    return skipped;
  }
  // Both release the bus once their reads are done, before the speaker write.
  const StreamResult result = _format == Format::Wav ? streamWav(speaker) : streamMp3(speaker);
  unlockBus();
  return result;
}

bool AudioSource::fail(const __FlashStringHelper *message) {
  closeFile();
  _error = message;
  return false;
}

bool AudioSource::readExact(uint8_t *buffer, size_t length) {
  return _file && _file.read(buffer, length) == length;
}

bool AudioSource::seekTo(uint32_t position) {
  return _file && _file.seek(position);
}

bool AudioSource::parseWav() {
  uint32_t fileSize = (uint32_t)_file.size();
  if (fileSize < 12 || !seekTo(0)) return fail(F("Invalid WAV header"));

  uint8_t riff[12];
  if (!readExact(riff, sizeof(riff)) || memcmp(riff, "RIFF", 4) != 0 ||
      memcmp(riff + 8, "WAVE", 4) != 0) {
    return fail(F("Invalid RIFF/WAVE file"));
  }

  uint32_t riffSize = readLe32(riff + 4);
  if (riffSize < 4 || riffSize > fileSize - 8) return fail(F("Invalid WAV bounds"));
  uint32_t riffEnd = 8 + riffSize;
  bool hasFormat = false;
  bool hasData = false;
  _wavInfoOffset = 0;
  _wavInfoSize = 0;
  _wavId3Offset = 0;
  _wavId3Size = 0;
  uint32_t cursor = 12;

  while (cursor <= riffEnd && riffEnd - cursor >= 8) {
    if (!seekTo(cursor)) return fail(F("Could not read WAV chunk"));
    uint8_t chunk[8];
    if (!readExact(chunk, sizeof(chunk))) return fail(F("Could not read WAV chunk"));
    uint32_t chunkSize = readLe32(chunk + 4);
    uint32_t payload = cursor + 8;
    if (chunkSize > riffEnd - payload) return fail(F("Invalid WAV chunk size"));

    if (memcmp(chunk, "fmt ", 4) == 0) {
      if (hasFormat || !parseFormatChunk(payload, chunkSize)) return false;
      hasFormat = true;
    } else if (memcmp(chunk, "data", 4) == 0) {
      if (hasData) return fail(F("Multiple WAV data chunks"));
      _dataOffset = payload;
      _dataBytes = chunkSize;
      hasData = true;
    } else if (memcmp(chunk, "LIST", 4) == 0 && _wavInfoSize == 0) {
      // AudioTags checks the list type and ignores lists other than INFO.
      _wavInfoOffset = payload;
      _wavInfoSize = chunkSize;
    } else if ((memcmp(chunk, "id3 ", 4) == 0 || memcmp(chunk, "ID3 ", 4) == 0) &&
               _wavId3Size == 0) {
      _wavId3Offset = payload;
      _wavId3Size = chunkSize;
    }

    uint32_t paddedSize = chunkSize + (chunkSize & 1U);
    if (paddedSize > riffEnd - payload) return fail(F("Invalid WAV padding"));
    cursor = payload + paddedSize;
  }

  if (cursor != riffEnd || !hasFormat || !hasData || _dataBytes == 0) {
    return fail(F("WAV fmt/data chunk missing"));
  }
  if (_blockAlign == 0 || _dataBytes % _blockAlign != 0) {
    return fail(F("WAV data is not frame-aligned"));
  }
  _frameCount = _dataBytes / _blockAlign;
  if (_frameCount == 0) return fail(F("WAV has no audio frames"));
  return true;
}

bool AudioSource::parseFormatChunk(uint32_t offset, uint32_t size) {
  if (size < 16 || size > 64 || !seekTo(offset)) return fail(F("Invalid WAV fmt chunk"));
  uint8_t fmt[64];
  memset(fmt, 0, sizeof(fmt));
  if (!readExact(fmt, size)) return fail(F("Could not read WAV fmt chunk"));

  uint16_t format = readLe16(fmt + 0);
  _channels = readLe16(fmt + 2);
  _sampleRate = readLe32(fmt + 4);
  uint32_t byteRate = readLe32(fmt + 8);
  _blockAlign = readLe16(fmt + 12);
  _bitsPerSample = readLe16(fmt + 14);
  if (_channels < 1 || _channels > 2 || _sampleRate == 0 ||
      (_bitsPerSample != 8 && _bitsPerSample != 16 &&
       _bitsPerSample != 24 && _bitsPerSample != 32)) {
    return fail(F("Unsupported WAV format"));
  }

  uint16_t bytesPerSample = (_bitsPerSample + 7) / 8;
  uint32_t expectedAlign = (uint32_t)_channels * bytesPerSample;
  uint64_t expectedRate = (uint64_t)_sampleRate * expectedAlign;
  if (_blockAlign != expectedAlign || expectedRate > 0xffffffffULL ||
      byteRate != (uint32_t)expectedRate) {
    return fail(F("Invalid WAV sample alignment"));
  }

  if (format == 1) return true;
  if (format != 0xfffe || size < 40 || readLe16(fmt + 18) < _bitsPerSample ||
      !isPcmSubFormat(fmt + 24)) {
    return fail(F("Unsupported WAV encoding"));
  }
  return true;
}

void AudioSource::beginTags() {
  _tags.begin(_sourceFileSize, readTagBytes, this);
  if (_format == Format::Mp3) {
    _tags.addId3v2(0, _sourceFileSize);
    _tags.addId3v1();
  } else if (_format == Format::Wav) {
    // An embedded ID3 tag outranks LIST/INFO where both name a field.
    if (_wavId3Size > 0) _tags.addId3v2(_wavId3Offset, _wavId3Size);
    if (_wavInfoSize > 0) _tags.addRiffInfo(_wavInfoOffset, _wavInfoSize);
  }
  _tagsParsing = true;
}

bool AudioSource::readTagBytes(void *context, uint32_t offset, uint8_t *buffer, size_t length) {
  AudioSource *source = static_cast<AudioSource *>(context);
  return source->seekTo(offset) && source->readExact(buffer, length);
}

bool AudioSource::beginMp3Analysis() {
  uint32_t fileSize = _sourceFileSize;
  if (fileSize < 4) return fail(F("Invalid MP3 file"));

  allocateSeekTable();
  if (loadMp3Cache()) return true;

  if (!lockBus(BusOpenWaitMs)) return fail(F("SD card busy"));
  const bool bounded = readMp3Bounds();
  unlockBus();
  if (!bounded) return false;

  _frameCount = 0;
  _framePosition = 0;
  _sampleRate = 0;
  _channels = 0;
  _bitsPerSample = 16;
  _mp3AnalysisPosition = _mp3DataOffset;
  _mp3FrameSamples = 0;
  _mp3ScanFrames = 0;
  _tocReady = false;
  if (_toc.attached()) _toc.clear();
  _mp3Analyzing = true;
  return true;
}

bool AudioSource::readMp3Bounds() {
  const uint32_t fileSize = _sourceFileSize;
  _mp3DataOffset = 0;
  _mp3AudioEnd = fileSize;
  uint8_t tag[10];
  if (fileSize >= sizeof(tag) && seekTo(0) && readExact(tag, sizeof(tag)) &&
      memcmp(tag, "ID3", 3) == 0) {
    uint32_t tagSize = readSynchsafe32(tag + 6);
    uint32_t footerBytes = (tag[5] & 0x10) ? 10 : 0;
    if (tagSize > fileSize - sizeof(tag) || footerBytes > fileSize - sizeof(tag) - tagSize) {
      return fail(F("Invalid MP3 ID3 tag"));
    }
    _mp3DataOffset = sizeof(tag) + tagSize + footerBytes;
  }

  if (fileSize >= 128 && seekTo(fileSize - 128)) {
    uint8_t id3v1[3];
    if (readExact(id3v1, sizeof(id3v1)) && memcmp(id3v1, "TAG", 3) == 0) {
      _mp3AudioEnd -= 128;
    }
  }
  if (_mp3DataOffset >= _mp3AudioEnd || _mp3AudioEnd - _mp3DataOffset < 4) {
    return fail(F("MP3 has no audio frames"));
  }
  return true;
}

// `.empa` v2: a 44-byte little-endian header bound to the source's size and
// modification time, then `tocCount` u32 seek-table offsets. See docs/media.md.
bool AudioSource::loadMp3Cache() {
  CachePaths paths;
  if (!CachePath::forSource(_path, FileFormats::Format::Empa, paths)) return false;
  StorageFile cache;
  if (_mediaCache.openRead(paths, _secretAccess, cache) != MediaCache::Result::Complete) return false;
  uint32_t cacheSize = 0;
  uint8_t header[Mp3CacheHeaderBytes];
  bool readOk = _mediaCache.fileSize(cache, cacheSize) == MediaCache::Result::Complete &&
    cacheSize >= Mp3CacheHeaderBytes &&
    cacheSize <= Mp3CacheHeaderBytes + sizeof(uint32_t) * Mp3SeekTable::MaxEntries &&
    _mediaCache.read(cache, header, sizeof(header)) == MediaCache::Result::Complete;
  const uint32_t tocInterval = readOk ? readLe32(header + 36) : 0;
  const uint32_t tocCount = readOk ? readLe32(header + 40) : 0;
  // Version 1 had no seek table; it is rejected and rebuilt by a rescan.
  readOk = readOk && header[0] == 'E' && header[1] == 'M' && header[2] == 'P' && header[3] == 'A' &&
    header[4] == Mp3CacheVersion && header[5] == 0 && readLe16(header + 6) == Mp3CacheHeaderBytes &&
    MediaCache::matches({readLe32(header + 8), readLe32(header + 12)},
                        {_sourceFileSize, _sourceWriteTime}) &&
    tocCount <= Mp3SeekTable::MaxEntries &&
    cacheSize == Mp3CacheHeaderBytes + sizeof(uint32_t) * tocCount;
  // Without seek-table memory the cache still saves the scan; the entries are skipped.
  const bool readToc = readOk && _toc.attached() && tocCount > 0;
  if (readToc) {
    readOk = _mediaCache.read(cache, _tocStorage, sizeof(uint32_t) * tocCount) ==
             MediaCache::Result::Complete;
    for (uint32_t i = 0; readOk && i < tocCount; i++) {
      _tocStorage[i] = readLe32(reinterpret_cast<const uint8_t *>(_tocStorage + i));
    }
  }
  _mediaCache.close(cache);
  if (!readOk) return false;

  uint32_t dataOffset = readLe32(header + 16);
  uint32_t audioEnd = readLe32(header + 20);
  uint32_t frameCount = readLe32(header + 24);
  uint32_t sampleRate = readLe32(header + 28);
  uint16_t channels = readLe16(header + 32);
  uint16_t frameSamples = readLe16(header + 34);
  if (dataOffset >= audioEnd || audioEnd > _sourceFileSize || frameCount == 0 ||
      sampleRate == 0 || (channels != 1 && channels != 2) ||
      (frameSamples != 576 && frameSamples != 1152) || frameCount % frameSamples != 0) {
    return false;
  }

  uint32_t firstFrameBytes = 0;
  uint16_t firstFrameSamples = 0;
  uint32_t firstSampleRate = 0;
  uint16_t firstChannels = 0;
  _mp3DataOffset = dataOffset;
  _mp3AudioEnd = audioEnd;
  if (!lockBus(BusOpenWaitMs)) return false;
  const bool parsed =
    parseMp3FrameHeader(dataOffset, firstFrameBytes, firstFrameSamples, firstSampleRate, firstChannels);
  unlockBus();
  if (!parsed || firstSampleRate != sampleRate || firstChannels != channels ||
      firstFrameSamples != frameSamples) {
    return false;
  }
  _tocReady = false;
  if (readToc &&
      !(_tocReady = _toc.adopt(tocInterval, (uint16_t)tocCount, dataOffset, audioEnd,
                               frameCount / frameSamples))) {
    Serial.print(F("MP3 cache seek table invalid: "));
    Serial.println(paths.finalPath);
    return false;
  }

  _frameCount = frameCount;
  _framePosition = 0;
  _sampleRate = sampleRate;
  _channels = channels;
  _bitsPerSample = 16;
  _mp3FrameSamples = frameSamples;
  _mp3Analyzing = false;
  Serial.print(F("MP3 cache hit: "));
  Serial.println(paths.finalPath);
  return true;
}

void AudioSource::writeMp3Cache() {
  if (!_file || _frameCount == 0 || _sampleRate == 0 || (_channels != 1 && _channels != 2)) return;
  if (!_tocReady) {
    // A cache without entries would fail validation; scan again next time.
    Serial.println(F("MP3 cache skipped: no seek table"));
    return;
  }

  CachePaths paths;
  if (!CachePath::forSource(_path, FileFormats::Format::Empa, paths)) return;
  MediaCache::Writer writer;
  if (_mediaCache.beginWrite(paths, _secretAccess, writer) != MediaCache::Result::Complete) {
    Serial.print(F("MP3 cache directory failed: "));
    Serial.println(paths.directory);
    return;
  }
  uint8_t header[Mp3CacheHeaderBytes] = {};
  header[0] = 'E'; header[1] = 'M'; header[2] = 'P'; header[3] = 'A';
  header[4] = Mp3CacheVersion;
  writeLe16(header + 6, Mp3CacheHeaderBytes);
  writeLe32(header + 8, _sourceFileSize);
  writeLe32(header + 12, _sourceWriteTime);
  writeLe32(header + 16, _mp3DataOffset);
  writeLe32(header + 20, _mp3AudioEnd);
  writeLe32(header + 24, _frameCount);
  writeLe32(header + 28, _sampleRate);
  writeLe16(header + 32, _channels);
  writeLe16(header + 34, _mp3FrameSamples);
  writeLe32(header + 36, _toc.interval());
  writeLe32(header + 40, _toc.count());
  bool written = _mediaCache.write(writer, header, sizeof(header)) == MediaCache::Result::Complete;
  uint8_t chunk[256];
  for (uint16_t i = 0; written && i < _toc.count();) {
    uint16_t entries = 0;
    for (; entries < sizeof(chunk) / 4 && i < _toc.count(); entries++, i++) {
      writeLe32(chunk + entries * 4, _toc.offsets()[i]);
    }
    written = _mediaCache.write(writer, chunk, entries * 4) == MediaCache::Result::Complete;
  }
  if (!written || _mediaCache.commit(writer) != MediaCache::Result::Complete) {
    _mediaCache.abort(writer);
    Serial.print(F("MP3 cache write failed: "));
    Serial.println(paths.finalPath);
    return;
  }
  Serial.printf("MP3 cache ready (%u seek entries every %lu frames): %s\n", _toc.count(),
                (unsigned long)_toc.interval(), paths.finalPath.c_str());
}

void AudioSource::analyzeMp3Step() {
  if (!_mp3Analyzing || failed()) return;

  for (uint8_t i = 0; i < Mp3AnalysisFramesPerStep; i++) {
    if (_mp3AnalysisPosition == _mp3AudioEnd) {
      _mp3Analyzing = false;
      if (_frameCount == 0) {
        fail(F("MP3 has no audio frames"));
      } else {
        _tocReady = _toc.attached() && _toc.complete() && _toc.count() > 0;
        _mp3CachePending = true;
      }
      return;
    }

    uint32_t frameBytes = 0;
    uint16_t frameSamples = 0;
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    if (!parseMp3FrameHeader(_mp3AnalysisPosition, frameBytes, frameSamples, sampleRate, channels)) {
      fail(F("Invalid MP3 frame"));
      return;
    }
    if (_mp3FrameSamples == 0) _mp3FrameSamples = frameSamples;
    _toc.addFrame(_mp3ScanFrames++, _mp3AnalysisPosition);
    if (_sampleRate == 0) {
      _sampleRate = sampleRate;
      _channels = channels;
    } else if (_sampleRate != sampleRate || _channels != channels) {
      fail(F("MP3 stream format changed"));
      return;
    }
    if (_frameCount > UINT32_MAX - frameSamples) {
      fail(F("MP3 is too long"));
      return;
    }
    _frameCount += frameSamples;
    _mp3AnalysisPosition += frameBytes;
  }
}

bool AudioSource::parseMp3FrameHeader(uint32_t position,
                                      uint32_t &frameBytes,
                                      uint16_t &frameSamples,
                                      uint32_t &sampleRate,
                                      uint16_t &channels) {
  frameBytes = 0;
  frameSamples = 0;
  sampleRate = 0;
  channels = 0;
  if (position > _mp3AudioEnd || _mp3AudioEnd - position < 4 || !seekTo(position)) return false;

  uint8_t bytes[4];
  if (!readExact(bytes, sizeof(bytes))) return false;
  uint32_t header = readBe32(bytes);
  uint8_t version = (header >> 19) & 0x03;
  uint8_t layer = (header >> 17) & 0x03;
  uint8_t bitrateIndex = (header >> 12) & 0x0f;
  uint8_t sampleRateIndex = (header >> 10) & 0x03;
  bool padded = ((header >> 9) & 0x01) != 0;
  uint8_t channelMode = (header >> 6) & 0x03;
  if ((header >> 21) != 0x07ff || version == 1 || layer != 1 ||
      bitrateIndex == 0 || bitrateIndex == 15 || sampleRateIndex == 3) {
    return false;
  }

  static const uint32_t mpeg1Rates[] = { 44100, 48000, 32000 };
  static const uint32_t mpeg2Rates[] = { 22050, 24000, 16000 };
  static const uint32_t mpeg25Rates[] = { 11025, 12000, 8000 };
  bool mpeg1 = version == 3;
  sampleRate = mpeg1 ? mpeg1Rates[sampleRateIndex]
                       : (version == 2 ? mpeg2Rates[sampleRateIndex] : mpeg25Rates[sampleRateIndex]);
  uint16_t bitrate = mpeg1 ? Mpeg1Layer3Bitrates[bitrateIndex] : Mpeg2Layer3Bitrates[bitrateIndex];
  frameSamples = mpeg1 ? 1152 : 576;
  uint64_t size = ((uint64_t)(mpeg1 ? 144 : 72) * bitrate * 1000) / sampleRate + (padded ? 1 : 0);
  if (size < 4 || size > _mp3AudioEnd - position) return false;
  frameBytes = (uint32_t)size;
  channels = channelMode == 3 ? 1 : 2;
  return true;
}

bool AudioSource::resetMp3Decoder(uint32_t offset) {
  if (!_file || !seekTo(offset)) return false;
  _mp3InputOffset = 0;
  _mp3InputBytes = 0;
  _mp3PcmFrames = 0;
  _mp3PcmPosition = 0;
  mp3dec_init(&_mp3Decoder);
  return true;
}

bool AudioSource::refillMp3Input() {
  if (!_file) return false;
  if (_mp3InputOffset > 0 && _mp3InputBytes > 0) {
    memmove(_mp3InputBuffer, _mp3InputBuffer + _mp3InputOffset, _mp3InputBytes);
  }
  _mp3InputOffset = 0;

  while (_mp3InputBytes < Mp3InputBufferBytes && (uint32_t)_file.position() < _mp3AudioEnd) {
    uint32_t remaining = _mp3AudioEnd - (uint32_t)_file.position();
    size_t room = Mp3InputBufferBytes - _mp3InputBytes;
    size_t request = remaining < room ? remaining : room;
    size_t count = _file.read(_mp3InputBuffer + _mp3InputBytes, request);
    if (count == 0) return false;
    _mp3InputBytes += count;
  }
  return _mp3InputBytes > 0;
}

bool AudioSource::decodeMp3Frame() {
  if (!refillMp3Input()) {
    if (_file && (uint32_t)_file.position() >= _mp3AudioEnd && _mp3InputBytes == 0) return false;
    return fail(F("MP3 read failed"));
  }
  // Decoding needs no SD access.
  unlockBus();

  mp3dec_frame_info_t info;
  memset(&info, 0, sizeof(info));
  int samples = mp3dec_decode_frame(&_mp3Decoder,
                                    _mp3InputBuffer + _mp3InputOffset,
                                    _mp3InputBytes,
                                    _mp3PcmBuffer,
                                    &info);
  if (info.frame_bytes < 0 || (uint32_t)info.frame_bytes > _mp3InputBytes) {
    return fail(F("MP3 decoder failed"));
  }
  if (info.frame_bytes > 0) {
    _mp3InputOffset += info.frame_bytes;
    _mp3InputBytes -= info.frame_bytes;
  }
  if (samples <= 0) {
    if (info.frame_bytes == 0) return fail(F("Truncated MP3 frame"));
    return fail(F("MP3 decode failed"));
  }
  if (info.layer != 3 || info.channels != _channels || (uint32_t)info.hz != _sampleRate ||
      samples > Mp3MaxPcmFrames / info.channels) {
    return fail(F("Unsupported MP3 frame"));
  }
  _mp3PcmFrames = samples;
  _mp3PcmPosition = 0;
  return true;
}

bool AudioSource::prepareStream() {
  if (_format == Format::Wav) {
    if (!seekTo(_dataOffset + (uint64_t)_framePosition * _blockAlign)) {
      return fail(F("Could not seek WAV data"));
    }
  } else if (_format == Format::Mp3) {
    if (!prepareMp3Position()) return false;
  } else {
    return fail(F("Unsupported audio file"));
  }
  _streamReady = true;
  return true;
}

AudioSource::StreamResult AudioSource::streamWav(SpeakerController &speaker) {
  uint32_t remaining = _frameCount - _framePosition;
  uint16_t frames = remaining > OutputFrames ? OutputFrames : (uint16_t)remaining;
  uint32_t bytes = (uint32_t)frames * _blockAlign;
  if (bytes > SourceBufferBytes || _file.read(_sourceBuffer, bytes) != bytes) {
    fail(F("WAV read failed"));
    return StreamResult::Failed;
  }
  unlockBus();

  uint16_t bytesPerSample = (_bitsPerSample + 7) / 8;
  for (uint16_t frame = 0; frame < frames; frame++) {
    const uint8_t *source = _sourceBuffer + (uint32_t)frame * _blockAlign;
    int64_t left = decodeSample(source);
    int64_t mono = left;
    if (_channels == 2) mono = (left + decodeSample(source + bytesPerSample)) / 2;
    _outputBuffer[frame] = (int16_t)clamp16(mono);
  }

  size_t written = speaker.writePcm16(_outputBuffer, frames, 100);
  if (written > frames) written = frames;
  _framePosition += (uint32_t)written;
  if (written == 0) {
    fail(F("Speaker write failed"));
    return StreamResult::Failed;
  }
  // A partial write left the file ahead of the stream; the next stream()
  // seeks back under the bus lock.
  if (written < frames) _streamReady = false;
  return StreamResult::Streaming;
}

bool AudioSource::prepareMp3Position() {
  _mp3SkipFrames = 0;
  _mp3SkipSamples = 0;
  uint32_t restart = _mp3DataOffset;
  if (_framePosition > 0) {
    // Without a seek table the decoder can only restart from the first frame.
    uint32_t entryFrame = 0;
    if (!_tocReady || _mp3FrameSamples == 0) return fail(F("MP3 position lost"));
    const uint32_t target = _framePosition / _mp3FrameSamples;
    if (!_toc.lookup(target, entryFrame, restart)) return fail(F("MP3 position lost"));
    // The frames from the entry up to the target are decoded and dropped;
    // the first of them only refills the bit reservoir.
    _mp3SkipFrames = target - entryFrame;
    _mp3SkipSamples = (uint16_t)(_framePosition % _mp3FrameSamples);
  }
  if (!resetMp3Decoder(restart)) return fail(F("MP3 decoder setup failed"));
  return true;
}

AudioSource::StreamResult AudioSource::discardMp3Frames() {
  for (uint8_t i = 0; i < Mp3DiscardFramesPerStep && _mp3SkipFrames > 0; i++) {
    if (!lockBus(BusWaitMs)) return StreamResult::Busy;
    if (!decodeMp3Frame()) {
      if (!failed()) fail(F("MP3 ended early"));
      return StreamResult::Failed;
    }
    _mp3PcmPosition = _mp3PcmFrames;
    _mp3SkipFrames--;
  }
  // Nothing reached the speaker yet; the next call plays the target frame.
  return StreamResult::Busy;
}

AudioSource::StreamResult AudioSource::streamMp3(SpeakerController &speaker) {
  if (_mp3PcmPosition >= _mp3PcmFrames) {
    if (!decodeMp3Frame()) {
      if (failed()) return StreamResult::Failed;
      // The stream ran out before the frame count the scan recorded.
      fail(F("MP3 ended early"));
      return StreamResult::Failed;
    }
    if (_mp3SkipSamples > 0) {
      // A seek landed inside this frame.
      _mp3PcmPosition = _mp3SkipSamples < _mp3PcmFrames ? _mp3SkipSamples : _mp3PcmFrames;
      _mp3SkipSamples = 0;
    }
  }

  unlockBus();
  if (_mp3PcmPosition >= _mp3PcmFrames) return StreamResult::Streaming;
  uint16_t remaining = _mp3PcmFrames - _mp3PcmPosition;
  uint16_t frames = remaining > OutputFrames ? OutputFrames : remaining;
  if (frames > _frameCount - _framePosition) frames = (uint16_t)(_frameCount - _framePosition);
  for (uint16_t frame = 0; frame < frames; frame++) {
    const mp3d_sample_t *source = _mp3PcmBuffer +
      (uint32_t)(_mp3PcmPosition + frame) * _channels;
    int32_t mono = source[0];
    if (_channels == 2) mono = ((int32_t)source[0] + (int32_t)source[1]) / 2;
    _outputBuffer[frame] = (int16_t)clamp16(mono);
  }

  size_t written = speaker.writePcm16(_outputBuffer, frames, 100);
  if (written > frames) written = frames;
  _mp3PcmPosition += written;
  _framePosition += written;
  if (written == 0) {
    fail(F("Speaker write failed"));
    return StreamResult::Failed;
  }
  return StreamResult::Streaming;
}

int32_t AudioSource::decodeSample(const uint8_t *sample) const {
  if (_bitsPerSample == 8) return ((int32_t)sample[0] - 128) << 8;
  if (_bitsPerSample == 16) {
    uint16_t raw = readLe16(sample);
    return (int16_t)raw;
  }
  if (_bitsPerSample == 24) {
    int32_t raw = (int32_t)sample[0] | ((int32_t)sample[1] << 8) | ((int32_t)sample[2] << 16);
    if (raw & 0x00800000) raw |= (int32_t)0xff000000;
    return raw >> 8;
  }
  int32_t raw = (int32_t)readLe32(sample);
  return raw >> 16;
}
