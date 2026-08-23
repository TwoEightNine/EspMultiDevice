#include "AudioPlayerScreen.h"

#include "Navigator.h"

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
}

AudioPlayerScreen::AudioPlayerScreen(Navigator &navigator,
                                     TouchController &touch,
                                     DisplayController &display,
                                     SdCardController &sdCard,
                                     SpeakerController &speaker)
  : Screen(navigator, touch, display),
    _sdCard(sdCard),
    _speaker(speaker),
    _state(State::Ready),
    _sdMounted(false),
    _lastTouchDown(false),
    _lastDrawMs(0),
    _finishAtMs(0),
    _dataOffset(0),
    _dataBytes(0),
    _frameCount(0),
    _framePosition(0),
    _sampleRate(0),
    _channels(0),
    _bitsPerSample(0),
    _blockAlign(0) {
}

void AudioPlayerScreen::onEnter(const ScreenData &data) {
  reset();
  _lastTouchDown = false;
  _path = data.kind == ScreenDataKind::AudioPath ? data.text : "";
  _fileName = displayName(_path);
  _sdMounted = _sdCard.isMounted();

  if (data.kind != ScreenDataKind::AudioPath || _path.length() == 0) {
    fail(F("No audio selected"));
  } else if (!_sdMounted) {
    fail(F("SD card unavailable"));
  } else {
    if (loadSource(_path)) {
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

  if (point.x >= 10 && point.x < BackTouchWidth && point.y >= 10 && point.y < 44) {
    _navigator.pop();
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
  handleSdCardEvent(_sdCard.update());
  if (_state == State::Playing) {
    if (_framePosition >= _frameCount) {
      if ((int32_t)(nowMs - _finishAtMs) >= 0) finishPlayback();
    } else {
      streamChunk();
    }
  }
  if (_state == State::Playing && nowMs - _lastDrawMs >= 200) {
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
  _lastDrawMs = 0;
  _finishAtMs = 0;
  _dataOffset = 0;
  _dataBytes = 0;
  _frameCount = 0;
  _framePosition = 0;
  _sampleRate = 0;
  _channels = 0;
  _bitsPerSample = 0;
  _blockAlign = 0;
}

bool AudioPlayerScreen::loadSource(const String &path) {
  if (_file) _file.close();
  _error = "";
  _state = State::Ready;
  _framePosition = 0;
  _finishAtMs = 0;
  _file = SD.open(path.c_str(), FILE_READ);
  if (!_file) return fail(F("Unable to open WAV file"));
  if (!parseWav()) {
    if (_file) _file.close();
    _state = State::Error;
    return false;
  }
  return true;
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
  if (!_file || _state == State::Error || !_sdMounted) return false;
  if (_state == State::Finished) _framePosition = 0;
  _finishAtMs = 0;
  if (!seekTo(_dataOffset + (uint64_t)_framePosition * _blockAlign)) {
    fail(F("Could not seek WAV data"));
    draw();
    return false;
  }
  if (!_speaker.isReady() && !_speaker.begin()) {
    _error = _speaker.lastError();
    _state = State::Error;
    stopSpeaker();
    if (_file) _file.close();
    draw();
    return false;
  }
  if (!_speaker.setSampleRate(_sampleRate)) {
    _error = _speaker.lastError();
    _state = State::Error;
    stopSpeaker();
    if (_file) _file.close();
    draw();
    return false;
  }
  _speaker.unmute();
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
  _speaker.mute();
}

void AudioPlayerScreen::streamChunk() {
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
    if (_path.length() > 0) loadSource(_path);
    draw();
  }
}

void AudioPlayerScreen::draw(bool force) {
  (void)force;
  if (!_display.isReady()) return;
  _display.tft().fillScreen(TFT_BLACK);
  drawTopBar();
  TFT_eSPI &tft = _display.tft();
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(truncateLabel(_fileName, _display.width() - 32), _display.width() / 2, 65, 2);
  tft.setTextDatum(TL_DATUM);
  drawPlayButton();
  drawDynamic(true);
  if (_state == State::Error) drawError();
}

void AudioPlayerScreen::drawTopBar() {
  TFT_eSPI &tft = _display.tft();
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawLine(18, 27, 34, 19, TFT_WHITE);
  tft.drawLine(18, 27, 34, 35, TFT_WHITE);
  tft.drawLine(18, 27, 48, 27, TFT_WHITE);
  tft.drawString(F("Audio"), 108, 16, 4);
}

void AudioPlayerScreen::drawDynamic(bool force) {
  if (!_display.isReady()) return;
  _lastDrawMs = millis();
  drawProgress();
  if (force || _state == State::Error) {
    // The error text is drawn separately so normal progress refreshes do not erase it.
    if (_state == State::Error) drawError();
  }
}

void AudioPlayerScreen::drawPlayButton() {
  TFT_eSPI &tft = _display.tft();
  const int16_t cx = _display.width() / 2;
  const int16_t cy = 118;
  bool enabled = _state != State::Error && _sdMounted;
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
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(timeText(_framePosition), x, 202, 2);
  tft.setTextDatum(TR_DATUM);
  tft.drawString(timeText(_frameCount), x + width, 202, 2);
  tft.setTextDatum(TL_DATUM);
}

void AudioPlayerScreen::drawError() {
  TFT_eSPI &tft = _display.tft();
  tft.fillRect(16, 238, _display.width() - 32, 28, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.drawString(_error, _display.width() / 2, 250, 2);
  tft.setTextDatum(TL_DATUM);
}

String AudioPlayerScreen::displayName(const String &path) const {
  int16_t slash = path.lastIndexOf('/');
  return slash < 0 ? path : path.substring(slash + 1);
}

String AudioPlayerScreen::truncateLabel(const String &label, int16_t maxWidth) {
  TFT_eSPI &tft = _display.tft();
  if (tft.textWidth(label, 2) <= maxWidth) return label;
  String result = label;
  while (result.length() > 3 && tft.textWidth(result + F("..."), 2) > maxWidth) {
    result.remove(result.length() - 1);
  }
  return result + F("...");
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
