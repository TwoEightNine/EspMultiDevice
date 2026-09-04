#define MINIMP3_ONLY_MP3
#define MINIMP3_IMPLEMENTATION
#include "third_party/minimp3/minimp3.h"

#include "AudioPlayerScreen.h"

#include "src/media/FileFormats.h"
#include "src/app/Navigator.h"
#include "src/ui/UiKit.h"

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

AudioPlayerScreen::AudioPlayerScreen(Navigator &navigator,
                                     TouchController &touch,
                                     DisplayController &display,
                                     SdCardController &sdCard,
                                     SpeakerController &speaker,
                                     AudioSettings &audioSettings,
                                     SecretStorage &secretStorage)
  : Screen(navigator, touch, display),
    _sdCard(sdCard),
    _speaker(speaker),
    _audioSettings(audioSettings),
    _secretStorage(secretStorage),
    _secretAccess(false),
    _state(State::Ready),
    _format(SourceFormat::None),
    _sdMounted(false),
    _lastTouchDown(false),
    _volumePickerVisible(false),
    _lastDrawMs(0),
    _finishAtMs(0),
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

void AudioPlayerScreen::onEnter(const ScreenData &data) {
  reset();
  _lastTouchDown = false;
  _volumePickerVisible = false;
  _secretAccess = data.fileAccess == FileAccessMode::Secret;
  _path = data.kind == ScreenDataKind::AudioPath ? data.text : "";
  _fileName = displayName(_path);
  _sdMounted = _sdCard.isMounted();

  // Present the player (including its loading affordance) before opening and
  // parsing the source file.  MP3 analysis itself remains incremental in
  // tick(), but WAV/header work can still take a visible moment on slow SD.
  _mp3Analyzing = true;
  draw();
  yield();
  _mp3Analyzing = false;

  if (data.kind != ScreenDataKind::AudioPath || _path.length() == 0) {
    fail(F("No audio selected"));
  } else if (!_sdMounted) {
    fail(F("SD card unavailable"));
  } else {
    if (loadSource(_path) && !_mp3Analyzing) {
      startPlayback();
    }
  }
  draw();
}

void AudioPlayerScreen::onExit() {
  stopSpeaker();
  if (_file) _file.close();
}

void AudioPlayerScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) {
    _lastTouchDown = false;
    return;
  }
  if (_lastTouchDown) return;
  _lastTouchDown = true;

  if (UiKit::navigationIconContains(point.x, point.y)) {
    _navigator.pop();
    return;
  }
  if (_volumePickerVisible) {
    int8_t level = volumePickerLevelAt(point.x, point.y);
    if (level >= AudioSettings::MinVolume) {
      _audioSettings.setVolume((uint8_t)level);
      _volumePickerVisible = false;
      draw();
    }
    return;
  }
  if (volumeButtonContains(point.x, point.y)) {
    _volumePickerVisible = true;
    drawVolumePicker();
    return;
  }
  if (controlContains(point.x, point.y)) {
    if (_state == State::Playing) {
      pausePlayback();
    } else if (_state == State::Ready || _state == State::Paused || _state == State::Finished) {
      startPlayback();
    }
  }
}

void AudioPlayerScreen::tick(uint32_t nowMs) {
  handleSdCardEvent(_sdCard.takeEvent());
  if (_mp3Analyzing) {
    analyzeMp3Step();
    return;
  }
  if (_state == State::Playing) {
    if (_framePosition >= _frameCount) {
      if ((int32_t)(nowMs - _finishAtMs) >= 0) finishPlayback();
    } else {
      streamChunk();
    }
  }
  if (_state == State::Playing && !_volumePickerVisible && nowMs - _lastDrawMs >= 200) {
    drawDynamic(false);
  }
}

void AudioPlayerScreen::reset() {
  stopSpeaker();
  if (_file) _file.close();
  _path = "";
  _fileName = "";
  _error = "";
  _state = State::Ready;
  _format = SourceFormat::None;
  _lastDrawMs = 0;
  _finishAtMs = 0;
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
  _mp3Analyzing = false;
  _mp3DataOffset = 0;
  _mp3AudioEnd = 0;
  _mp3AnalysisPosition = 0;
  _mp3InputOffset = 0;
  _mp3InputBytes = 0;
  _mp3PcmFrames = 0;
  _mp3PcmPosition = 0;
  mp3dec_init(&_mp3Decoder);
}

bool AudioPlayerScreen::loadSource(const String &path) {
  if (_file) _file.close();
  _error = "";
  _state = State::Ready;
  _framePosition = 0;
  _finishAtMs = 0;
  _format = formatForPath(path);
  _file = _secretStorage.open(path, FILE_READ, _secretAccess);
  if (!_file) return fail(F("Unable to open audio file"));
  _sourceFileSize = (uint32_t)_file.size();
  _sourceWriteTime = (uint32_t)_file.getLastWrite();
  if (_format == SourceFormat::Wav && !parseWav()) {
    if (_file) _file.close();
    _state = State::Error;
    return false;
  }
  if (_format == SourceFormat::Mp3 && !beginMp3Analysis()) {
    if (_file) _file.close();
    _state = State::Error;
    return false;
  }
  if (_format == SourceFormat::None) return fail(F("Unsupported audio file"));
  return true;
}

AudioPlayerScreen::SourceFormat AudioPlayerScreen::formatForPath(const String &path) const {
  switch (FileFormats::fromPath(path)) {
    case FileFormats::Format::Wav: return SourceFormat::Wav;
    case FileFormats::Format::Mp3: return SourceFormat::Mp3;
    default: break;
  }
  return SourceFormat::None;
}

bool AudioPlayerScreen::parseWav() {
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
  if (_frameCount == 0 || !seekTo(_dataOffset)) return fail(F("WAV has no audio frames"));
  return true;
}

bool AudioPlayerScreen::parseFormatChunk(uint32_t offset, uint32_t size) {
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

bool AudioPlayerScreen::beginMp3Analysis() {
  uint32_t fileSize = _sourceFileSize;
  if (fileSize < 4 || !seekTo(0)) return fail(F("Invalid MP3 file"));

  if (loadMp3Cache()) return true;

  _mp3DataOffset = 0;
  _mp3AudioEnd = fileSize;
  uint8_t tag[10];
  if (fileSize >= sizeof(tag) && readExact(tag, sizeof(tag)) && memcmp(tag, "ID3", 3) == 0) {
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

String AudioPlayerScreen::mp3CachePath() const {
  int slash = _path.lastIndexOf('/');
  String directory = slash <= 0 ? F("/") : _path.substring(0, slash);
  String name = slash < 0 ? _path : _path.substring(slash + 1);
  return directory + (directory == F("/") ? F("") : F("/")) + F(".cache/") + name + F(".empa");
}

bool AudioPlayerScreen::loadMp3Cache() {
  String path = mp3CachePath();
  if (!_secretStorage.exists(path)) return false;

  StorageFile cache = _secretStorage.open(path, FILE_READ, _secretAccess);
  uint8_t header[Mp3CacheHeaderBytes];
  bool readOk = cache && cache.size() == Mp3CacheHeaderBytes &&
    cache.read(header, sizeof(header)) == sizeof(header);
  if (cache) cache.close();
  if (!readOk || header[0] != 'E' || header[1] != 'M' || header[2] != 'P' || header[3] != 'A' ||
      header[4] != 1 || header[5] != 0 || readLe16(header + 6) != Mp3CacheHeaderBytes ||
      readLe32(header + 8) != _sourceFileSize || readLe32(header + 12) != _sourceWriteTime) {
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
  if (!resetMp3Decoder()) return false;
  Serial.print(F("MP3 cache hit: "));
  Serial.println(path);
  return true;
}

void AudioPlayerScreen::writeMp3Cache() {
  if (!_file || _frameCount == 0 || _sampleRate == 0 || (_channels != 1 && _channels != 2)) return;

  String path = mp3CachePath();
  int slash = path.lastIndexOf('/');
  String directory = slash <= 0 ? F("/") : path.substring(0, slash);
  if (!_secretStorage.mkdir(directory)) {
    Serial.print(F("MP3 cache directory failed: "));
    Serial.println(directory);
    return;
  }

  String tempPath = path + F(".tmp");
  _secretStorage.remove(tempPath);
  StorageFile cache = _secretStorage.open(tempPath, FILE_WRITE, _secretAccess);
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
  bool written = cache && cache.write(header, sizeof(header)) == sizeof(header);
  if (cache) cache.close();
  if (!written || (_secretStorage.exists(path) && !_secretStorage.remove(path)) ||
      !_secretStorage.rename(tempPath, path)) {
    _secretStorage.remove(tempPath);
    Serial.print(F("MP3 cache write failed: "));
    Serial.println(path);
    return;
  }
  Serial.print(F("MP3 cache ready: "));
  Serial.println(path);
}

void AudioPlayerScreen::analyzeMp3Step() {
  if (!_mp3Analyzing || _state == State::Error) return;

  for (uint8_t i = 0; i < Mp3AnalysisFramesPerTick; i++) {
    if (_mp3AnalysisPosition == _mp3AudioEnd) {
      _mp3Analyzing = false;
      if (_frameCount == 0 || !resetMp3Decoder()) {
        if (_state != State::Error) fail(F("MP3 decoder setup failed"));
      } else {
        writeMp3Cache();
        startPlayback();
      }
      draw();
      return;
    }

    uint32_t frameBytes = 0;
    uint16_t frameSamples = 0;
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    if (!parseMp3FrameHeader(_mp3AnalysisPosition, frameBytes, frameSamples, sampleRate, channels)) {
      fail(F("Invalid MP3 frame"));
      draw();
      return;
    }
    if (_sampleRate == 0) {
      _sampleRate = sampleRate;
      _channels = channels;
    } else if (_sampleRate != sampleRate || _channels != channels) {
      fail(F("MP3 stream format changed"));
      draw();
      return;
    }
    if (_frameCount > UINT32_MAX - frameSamples) {
      fail(F("MP3 is too long"));
      draw();
      return;
    }
    _frameCount += frameSamples;
    _mp3AnalysisPosition += frameBytes;
  }
}

bool AudioPlayerScreen::parseMp3FrameHeader(uint32_t position,
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

bool AudioPlayerScreen::resetMp3Decoder() {
  if (!_file || !seekTo(_mp3DataOffset)) return false;
  _mp3InputOffset = 0;
  _mp3InputBytes = 0;
  _mp3PcmFrames = 0;
  _mp3PcmPosition = 0;
  mp3dec_init(&_mp3Decoder);
  return true;
}

bool AudioPlayerScreen::refillMp3Input() {
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

bool AudioPlayerScreen::decodeMp3Frame() {
  while (true) {
    if (!refillMp3Input()) {
      if ((uint32_t)_file.position() >= _mp3AudioEnd && _mp3InputBytes == 0) return false;
      fail(F("MP3 read failed"));
      return false;
    }

    mp3dec_frame_info_t info;
    memset(&info, 0, sizeof(info));
    int samples = mp3dec_decode_frame(&_mp3Decoder,
                                      _mp3InputBuffer + _mp3InputOffset,
                                      _mp3InputBytes,
                                      _mp3PcmBuffer,
                                      &info);
    if (info.frame_bytes < 0 || (uint32_t)info.frame_bytes > _mp3InputBytes) {
      fail(F("MP3 decoder failed"));
      return false;
    }
    if (info.frame_bytes > 0) {
      _mp3InputOffset += info.frame_bytes;
      _mp3InputBytes -= info.frame_bytes;
    }
    if (samples <= 0) {
      if (info.frame_bytes == 0) {
        fail(F("Truncated MP3 frame"));
        return false;
      }
      fail(F("MP3 decode failed"));
      return false;
    }
    if (info.layer != 3 || info.channels != _channels || (uint32_t)info.hz != _sampleRate ||
        samples > Mp3MaxPcmFrames / info.channels) {
      fail(F("Unsupported MP3 frame"));
      return false;
    }
    _mp3PcmFrames = samples;
    _mp3PcmPosition = 0;
    return true;
  }
}

bool AudioPlayerScreen::fail(const __FlashStringHelper *message) {
  stopSpeaker();
  if (_file) _file.close();
  _error = message;
  _state = State::Error;
  return false;
}

bool AudioPlayerScreen::readExact(uint8_t *buffer, size_t length) {
  return _file && _file.read(buffer, length) == length;
}

bool AudioPlayerScreen::seekTo(uint32_t position) {
  return _file && _file.seek(position);
}

bool AudioPlayerScreen::startPlayback() {
  if (!_file || _state == State::Error || !_sdMounted || _mp3Analyzing) return false;
  if (_state == State::Finished) {
    _framePosition = 0;
    if (_format == SourceFormat::Mp3 && !resetMp3Decoder()) {
      fail(F("Could not restart MP3"));
      draw();
      return false;
    }
  }
  _finishAtMs = 0;
  if (_format == SourceFormat::Wav &&
      !seekTo(_dataOffset + (uint64_t)_framePosition * _blockAlign)) {
    fail(F("Could not seek WAV data"));
    draw();
    return false;
  }
  if (!_speaker.start(_sampleRate)) {
    _error = _speaker.lastError();
    _state = State::Error;
    stopSpeaker();
    if (_file) _file.close();
    draw();
    return false;
  }
  _state = State::Playing;
  drawPlayButton();
  return true;
}

void AudioPlayerScreen::pausePlayback() {
  stopSpeaker();
  _state = State::Paused;
  drawPlayButton();
}

void AudioPlayerScreen::finishPlayback() {
  _framePosition = _frameCount;
  stopSpeaker();
  _state = State::Finished;
  drawDynamic(true);
  drawPlayButton();
}

void AudioPlayerScreen::stopSpeaker() {
  _speaker.stop();
}

void AudioPlayerScreen::streamChunk() {
  if (_format == SourceFormat::Wav) {
    streamWavChunk();
  } else if (_format == SourceFormat::Mp3) {
    streamMp3Chunk();
  } else {
    fail(F("Unsupported audio file"));
    draw();
  }
}

void AudioPlayerScreen::streamWavChunk() {
  if (!_file || _framePosition >= _frameCount) {
    finishPlayback();
    return;
  }

  uint32_t remaining = _frameCount - _framePosition;
  uint16_t frames = remaining > OutputFrames ? OutputFrames : (uint16_t)remaining;
  uint32_t bytes = (uint32_t)frames * _blockAlign;
  if (bytes > SourceBufferBytes || _file.read(_sourceBuffer, bytes) != bytes) {
    stopSpeaker();
    fail(F("WAV read failed"));
    draw();
    return;
  }

  uint16_t bytesPerSample = (_bitsPerSample + 7) / 8;
  for (uint16_t frame = 0; frame < frames; frame++) {
    const uint8_t *source = _sourceBuffer + (uint32_t)frame * _blockAlign;
    int64_t left = decodeSample(source);
    int64_t mono = left;
    if (_channels == 2) mono = (left + decodeSample(source + bytesPerSample)) / 2;
    _outputBuffer[frame] = (int16_t)clamp16(mono);
  }

  size_t written = _speaker.writePcm16(_outputBuffer, frames, 100);
  if (written > frames) written = frames;
  _framePosition += (uint32_t)written;
  if (written < frames && !seekTo(_dataOffset + (uint64_t)_framePosition * _blockAlign)) {
    stopSpeaker();
    fail(F("Could not resume WAV stream"));
    draw();
    return;
  }
  if (written == 0) {
    stopSpeaker();
    fail(F("Speaker write failed"));
    draw();
  } else if (_framePosition >= _frameCount) {
    // Let the speaker's DMA queue drain before uninstalling the I2S driver.
    _finishAtMs = millis() + 100;
  }
}

void AudioPlayerScreen::streamMp3Chunk() {
  if (!_file || _framePosition >= _frameCount) {
    finishPlayback();
    return;
  }
  if (_mp3PcmPosition >= _mp3PcmFrames && !decodeMp3Frame()) {
    if (_state != State::Error) {
      if (_framePosition == _frameCount) {
        _finishAtMs = millis() + 100;
      } else {
        fail(F("MP3 ended early"));
        draw();
      }
    }
    return;
  }

  uint16_t remaining = _mp3PcmFrames - _mp3PcmPosition;
  uint16_t frames = remaining > OutputFrames ? OutputFrames : remaining;
  for (uint16_t frame = 0; frame < frames; frame++) {
    const mp3d_sample_t *source = _mp3PcmBuffer +
      (uint32_t)(_mp3PcmPosition + frame) * _channels;
    int32_t mono = source[0];
    if (_channels == 2) mono = ((int32_t)source[0] + (int32_t)source[1]) / 2;
    _outputBuffer[frame] = (int16_t)clamp16(mono);
  }

  size_t written = _speaker.writePcm16(_outputBuffer, frames, 100);
  if (written > frames) written = frames;
  _mp3PcmPosition += written;
  _framePosition += written;
  if (written == 0) {
    stopSpeaker();
    fail(F("Speaker write failed"));
    draw();
  } else if (_framePosition >= _frameCount) {
    // Let the speaker's DMA queue drain before uninstalling the I2S driver.
    _finishAtMs = millis() + 100;
  }
}

int32_t AudioPlayerScreen::decodeSample(const uint8_t *sample) const {
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

void AudioPlayerScreen::handleSdCardEvent(SdCardEvent event) {
  if (event == SdCardEvent::None) return;
  if (event == SdCardEvent::Removed || event == SdCardEvent::MountFailed) {
    stopSpeaker();
    if (_file) _file.close();
    _sdMounted = false;
    fail(event == SdCardEvent::Removed ? F("SD card removed") : F("SD card unavailable"));
    draw();
    return;
  }
  if (event == SdCardEvent::Mounted) {
    _sdMounted = true;
    if (_path.length() > 0 && loadSource(_path) && !_mp3Analyzing) startPlayback();
    draw();
  }
}

void AudioPlayerScreen::draw(bool force) {
  (void)force;
  if (!_display.isReady()) return;
  _display.tft().fillScreen(TFT_BLACK);
  drawTopBar();
  TFT_eSPI &tft = _display.tft();
  UiText::draw(tft, truncateLabel(_fileName, _display.width() - 32), _display.width() / 2, 65,
               TFT_WHITE, TFT_BLACK, UiText::Size::Title, TC_DATUM);
  drawPlayButton();
  drawVolumeButton();
  drawDynamic(true);
  if (_volumePickerVisible) drawVolumePicker();
  if (_state == State::Error) drawError();
}

void AudioPlayerScreen::drawTopBar() {
  UiKit::drawTopAppBar(_display.tft(), _display.width(), F("Audio"));
}

void AudioPlayerScreen::drawDynamic(bool force) {
  if (!_display.isReady()) return;
  _lastDrawMs = millis();
  drawProgress();
  drawStatus();
  if (force || _state == State::Error) {
    // The error text is drawn separately so normal progress refreshes do not erase it.
    if (_state == State::Error) drawError();
  }
}

void AudioPlayerScreen::drawStatus() {
  TFT_eSPI &tft = _display.tft();
  tft.fillRect(16, 238, _display.width() - 32, 28, TFT_BLACK);
  if (!_mp3Analyzing) return;
  UiText::draw(tft, F("Loading MP3..."), _display.width() / 2, 250,
               TFT_LIGHTGREY, TFT_BLACK, UiText::Size::Compact, MC_DATUM);
}

void AudioPlayerScreen::drawPlayButton() {
  TFT_eSPI &tft = _display.tft();
  const int16_t cx = _display.width() / 2;
  const int16_t cy = 118;
  bool enabled = _state != State::Error && _sdMounted && !_mp3Analyzing;
  uint16_t fill = enabled ? TFT_DARKGREY : TFT_BLACK;
  tft.fillCircle(cx, cy, 24, fill);
  tft.drawCircle(cx, cy, 24, enabled ? TFT_WHITE : TFT_DARKGREY);
  if (!enabled) return;
  tft.setTextColor(TFT_WHITE, fill);
  if (_state == State::Playing) {
    tft.fillRect(cx - 9, cy - 10, 6, 20, TFT_WHITE);
    tft.fillRect(cx + 3, cy - 10, 6, 20, TFT_WHITE);
  } else {
    tft.fillTriangle(cx - 7, cy - 11, cx - 7, cy + 11, cx + 11, cy, TFT_WHITE);
  }
}

void AudioPlayerScreen::drawVolumeButton() {
  TFT_eSPI &tft = _display.tft();
  const int16_t x = 20;
  const int16_t y = 142;
  const int16_t width = _display.width() - 40;
  const int16_t height = 28;
  tft.fillSmoothRoundRect(x, y, width, height, 5, TFT_DARKGREY, TFT_BLACK);
  String label = F("Volume: ");
  label += _audioSettings.volume();
  label += F(" / 10");
  UiText::draw(tft, label, _display.width() / 2, y + height / 2, TFT_WHITE, TFT_DARKGREY,
               UiText::Size::Content, MC_DATUM);
}

void AudioPlayerScreen::drawVolumePicker() {
  TFT_eSPI &tft = _display.tft();
  const uint8_t columns = 6;
  const int16_t panelX = 16;
  const int16_t panelY = 54;
  const int16_t panelWidth = _display.width() - panelX * 2;
  const int16_t panelHeight = 206;
  const int16_t gridLeft = panelX + 16;
  const int16_t gridTop = panelY + 60;
  const int16_t gap = 8;
  const int16_t cellHeight = 44;
  const int16_t availableWidth = panelWidth - 32 - gap * (columns - 1);
  const int16_t cellWidth = availableWidth / columns;
  const uint8_t selected = _audioSettings.volume();

  tft.fillRoundRect(panelX, panelY, panelWidth, panelHeight, 8, TFT_BLACK);
  tft.drawRoundRect(panelX, panelY, panelWidth, panelHeight, 8, TFT_WHITE);
  String title = F("Volume ");
  title += selected;
  title += F(" / 10");
  UiText::draw(tft, title, _display.width() / 2, panelY + 28, TFT_WHITE, TFT_BLACK,
               UiText::Size::Title, MC_DATUM);

  for (uint8_t level = AudioSettings::MinVolume; level <= AudioSettings::MaxVolume; level++) {
    uint8_t column = level % columns;
    uint8_t row = level / columns;
    int16_t x = gridLeft + column * (cellWidth + gap);
    int16_t y = gridTop + row * (cellHeight + gap);
    bool isSelected = level == selected;
    uint16_t fill = isSelected ? TFT_CYAN : TFT_DARKGREY;
    tft.fillSmoothRoundRect(x, y, cellWidth, cellHeight, 5, fill, TFT_BLACK);
    UiText::draw(tft, String(level), x + cellWidth / 2, y + cellHeight / 2,
                 isSelected ? TFT_BLACK : TFT_WHITE, fill, UiText::Size::Content, MC_DATUM);
  }
}

void AudioPlayerScreen::drawProgress() {
  TFT_eSPI &tft = _display.tft();
  const int16_t x = 20;
  const int16_t y = 178;
  const int16_t width = _display.width() - 40;
  const int16_t height = 8;
  uint32_t filled = 0;
  if (_frameCount > 0 && _framePosition > 0) {
    filled = (uint32_t)(((uint64_t)width * _framePosition) / _frameCount);
    if (filled > (uint32_t)width) filled = width;
  }
  tft.fillRect(x, y, width, height, TFT_DARKGREY);
  if (filled > 0) tft.fillRect(x, y, filled, height, TFT_CYAN);
  UiText::draw(tft, timeText(_framePosition), x, 202, TFT_WHITE, TFT_BLACK,
               UiText::Size::Compact);
  UiText::draw(tft, timeText(_frameCount), x + width, 202, TFT_WHITE, TFT_BLACK,
               UiText::Size::Compact, TR_DATUM);
}

void AudioPlayerScreen::drawError() {
  TFT_eSPI &tft = _display.tft();
  tft.fillRect(16, 238, _display.width() - 32, 28, TFT_BLACK);
  UiText::draw(tft, _error, _display.width() / 2, 250, TFT_RED, TFT_BLACK,
               UiText::Size::Compact, MC_DATUM);
}

String AudioPlayerScreen::displayName(const String &path) const {
  int16_t slash = path.lastIndexOf('/');
  return slash < 0 ? path : path.substring(slash + 1);
}

String AudioPlayerScreen::truncateLabel(const String &label, int16_t maxWidth) {
  return UiText::ellipsize(label, maxWidth, UiText::Size::Title);
}

String AudioPlayerScreen::timeText(uint32_t frames) const {
  uint32_t seconds = _sampleRate == 0 ? 0 : frames / _sampleRate;
  char buffer[20];
  snprintf(buffer, sizeof(buffer), "%lu:%02lu",
           (unsigned long)(seconds / 60), (unsigned long)(seconds % 60));
  return String(buffer);
}

bool AudioPlayerScreen::controlContains(int16_t x, int16_t y) const {
  int16_t left = _display.width() / 2 - 24;
  return x >= left && x < left + 48 && y >= 94 && y < 142;
}

bool AudioPlayerScreen::volumeButtonContains(int16_t x, int16_t y) const {
  return x >= 20 && x < _display.width() - 20 && y >= 142 && y < 170;
}

int8_t AudioPlayerScreen::volumePickerLevelAt(int16_t x, int16_t y) const {
  const uint8_t columns = 6;
  const int16_t panelX = 16;
  const int16_t panelY = 54;
  const int16_t panelWidth = _display.width() - panelX * 2;
  const int16_t gridLeft = panelX + 16;
  const int16_t gridTop = panelY + 60;
  const int16_t gap = 8;
  const int16_t cellHeight = 44;
  const int16_t availableWidth = panelWidth - 32 - gap * (columns - 1);
  const int16_t cellWidth = availableWidth / columns;
  for (uint8_t level = AudioSettings::MinVolume; level <= AudioSettings::MaxVolume; level++) {
    uint8_t column = level % columns;
    uint8_t row = level / columns;
    int16_t left = gridLeft + column * (cellWidth + gap);
    int16_t top = gridTop + row * (cellHeight + gap);
    if (x >= left && x < left + cellWidth && y >= top && y < top + cellHeight) {
      return (int8_t)level;
    }
  }
  return -1;
}
