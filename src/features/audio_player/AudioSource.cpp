#define MINIMP3_ONLY_MP3
#define MINIMP3_IMPLEMENTATION
#include "../../../third_party/minimp3/minimp3.h"

#include "AudioSource.h"

#include "../../media/FileFormats.h"
#include "../../services/audio/SpeakerController.h"

#include <SD.h>
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
    _mp3PcmPosition(0) {
  mp3dec_init(&_mp3Decoder);
}

void AudioSource::closeFile() {
  if (_file) _file.close();
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
  mp3dec_init(&_mp3Decoder);
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
  _file = _secretStorage.open(path, FILE_READ, _secretAccess);
  if (!_file) return fail(F("Unable to open audio file"));
  SourceFingerprint fingerprint{};
  if (_mediaCache.captureFingerprint(_file, fingerprint) != MediaCache::Result::Complete) {
    return fail(F("Unable to inspect audio file"));
  }
  _sourceFileSize = fingerprint.size;
  _sourceWriteTime = fingerprint.modified;
  if (_format == Format::Wav && !parseWav()) return false;
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
    return;
  }
  if (_mp3Analyzing) analyzeMp3Step();
}

uint32_t AudioSource::bitrateKbps() const {
  if (_format == Format::Wav) return (uint32_t)(((uint64_t)_sampleRate * _blockAlign * 8) / 1000);
  if (_format != Format::Mp3 || _frameCount == 0 || _sampleRate == 0 || loading()) return 0;
  const uint64_t bits = (uint64_t)(_mp3AudioEnd - _mp3DataOffset) * 8;
  return (uint32_t)((bits * _sampleRate / _frameCount + 500) / 1000);
}

bool AudioSource::canSeek() const {
  return _file && !loading() && _format == Format::Wav && _frameCount > 0;
}

bool AudioSource::seek(uint32_t frame) {
  if (!_file || loading()) return false;
  if (frame > _frameCount) frame = _frameCount;
  if (_format == Format::Mp3 && frame != 0) return false;
  _framePosition = frame;
  _mp3PcmFrames = 0;
  _mp3PcmPosition = 0;
  _streamReady = false;
  return true;
}

AudioSource::StreamResult AudioSource::stream(SpeakerController &speaker) {
  if (!_file || failed()) return StreamResult::Failed;
  if (ended()) return StreamResult::Ended;
  if (!_streamReady && !prepareStream()) return StreamResult::Failed;
  return _format == Format::Wav ? streamWav(speaker) : streamMp3(speaker);
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
  if (fileSize < 4 || !seekTo(0)) return fail(F("Invalid MP3 file"));

  if (loadMp3Cache()) return true;

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

  _frameCount = 0;
  _framePosition = 0;
  _sampleRate = 0;
  _channels = 0;
  _bitsPerSample = 16;
  _mp3AnalysisPosition = _mp3DataOffset;
  _mp3Analyzing = true;
  return true;
}

bool AudioSource::loadMp3Cache() {
  CachePaths paths;
  if (!CachePath::forSource(_path, FileFormats::Format::Empa, paths)) return false;
  StorageFile cache;
  if (_mediaCache.openRead(paths, _secretAccess, cache) != MediaCache::Result::Complete) return false;
  uint32_t cacheSize = 0;
  uint8_t header[Mp3CacheHeaderBytes];
  bool readOk = _mediaCache.fileSize(cache, cacheSize) == MediaCache::Result::Complete &&
    cacheSize == Mp3CacheHeaderBytes &&
    _mediaCache.read(cache, header, sizeof(header)) == MediaCache::Result::Complete;
  _mediaCache.close(cache);
  if (!readOk || header[0] != 'E' || header[1] != 'M' || header[2] != 'P' || header[3] != 'A' ||
      header[4] != 1 || header[5] != 0 || readLe16(header + 6) != Mp3CacheHeaderBytes ||
      !MediaCache::matches({readLe32(header + 8), readLe32(header + 12)},
                           {_sourceFileSize, _sourceWriteTime})) {
    return false;
  }

  uint32_t dataOffset = readLe32(header + 16);
  uint32_t audioEnd = readLe32(header + 20);
  uint32_t frameCount = readLe32(header + 24);
  uint32_t sampleRate = readLe32(header + 28);
  uint16_t channels = readLe16(header + 32);
  if (dataOffset >= audioEnd || audioEnd > _sourceFileSize || frameCount == 0 ||
      sampleRate == 0 || (channels != 1 && channels != 2)) {
    return false;
  }

  _mp3DataOffset = dataOffset;
  _mp3AudioEnd = audioEnd;
  uint32_t firstFrameBytes = 0;
  uint16_t firstFrameSamples = 0;
  uint32_t firstSampleRate = 0;
  uint16_t firstChannels = 0;
  if (!parseMp3FrameHeader(dataOffset, firstFrameBytes, firstFrameSamples, firstSampleRate, firstChannels) ||
      firstSampleRate != sampleRate || firstChannels != channels || firstFrameSamples > frameCount) {
    return false;
  }

  _frameCount = frameCount;
  _framePosition = 0;
  _sampleRate = sampleRate;
  _channels = channels;
  _bitsPerSample = 16;
  _mp3Analyzing = false;
  Serial.print(F("MP3 cache hit: "));
  Serial.println(paths.finalPath);
  return true;
}

void AudioSource::writeMp3Cache() {
  if (!_file || _frameCount == 0 || _sampleRate == 0 || (_channels != 1 && _channels != 2)) return;

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
  header[4] = 1;
  writeLe16(header + 6, Mp3CacheHeaderBytes);
  writeLe32(header + 8, _sourceFileSize);
  writeLe32(header + 12, _sourceWriteTime);
  writeLe32(header + 16, _mp3DataOffset);
  writeLe32(header + 20, _mp3AudioEnd);
  writeLe32(header + 24, _frameCount);
  writeLe32(header + 28, _sampleRate);
  writeLe16(header + 32, _channels);
  bool written = _mediaCache.write(writer, header, sizeof(header)) == MediaCache::Result::Complete;
  if (!written || _mediaCache.commit(writer) != MediaCache::Result::Complete) {
    _mediaCache.abort(writer);
    Serial.print(F("MP3 cache write failed: "));
    Serial.println(paths.finalPath);
    return;
  }
  Serial.print(F("MP3 cache ready: "));
  Serial.println(paths.finalPath);
}

void AudioSource::analyzeMp3Step() {
  if (!_mp3Analyzing || failed()) return;

  for (uint8_t i = 0; i < Mp3AnalysisFramesPerStep; i++) {
    if (_mp3AnalysisPosition == _mp3AudioEnd) {
      _mp3Analyzing = false;
      if (_frameCount == 0) {
        fail(F("MP3 has no audio frames"));
      } else {
        writeMp3Cache();
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

bool AudioSource::resetMp3Decoder() {
  if (!_file || !seekTo(_mp3DataOffset)) return false;
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
    // Without a seek table the decoder can only restart from the first frame.
    if (_framePosition != 0) return fail(F("MP3 position lost"));
    if (!resetMp3Decoder()) return fail(F("MP3 decoder setup failed"));
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
  if (written < frames && !seekTo(_dataOffset + (uint64_t)_framePosition * _blockAlign)) {
    fail(F("Could not resume WAV stream"));
    return StreamResult::Failed;
  }
  return StreamResult::Streaming;
}

AudioSource::StreamResult AudioSource::streamMp3(SpeakerController &speaker) {
  if (_mp3PcmPosition >= _mp3PcmFrames && !decodeMp3Frame()) {
    if (failed()) return StreamResult::Failed;
    // The stream ran out before the frame count the scan recorded.
    fail(F("MP3 ended early"));
    return StreamResult::Failed;
  }

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
