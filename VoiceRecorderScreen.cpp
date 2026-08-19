#include "VoiceRecorderScreen.h"

#include "Navigator.h"

#include <SD.h>
#include <string.h>

namespace {
void putLe16(uint8_t *buffer, uint16_t value) {
  buffer[0] = value & 0xff;
  buffer[1] = (value >> 8) & 0xff;
}

void putLe32(uint8_t *buffer, uint32_t value) {
  buffer[0] = value & 0xff;
  buffer[1] = (value >> 8) & 0xff;
  buffer[2] = (value >> 16) & 0xff;
  buffer[3] = (value >> 24) & 0xff;
}

uint16_t readLe16(const uint8_t *buffer) {
  return (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8);
}

uint32_t readLe32(const uint8_t *buffer) {
  return (uint32_t)buffer[0]
    | ((uint32_t)buffer[1] << 8)
    | ((uint32_t)buffer[2] << 16)
    | ((uint32_t)buffer[3] << 24);
}
}

const char *VoiceRecorderScreen::RecordingPath = "/voice_last.wav";

VoiceRecorderScreen::VoiceRecorderScreen(Navigator &navigator,
                                         TouchController &touch,
                                         DisplayController &display,
                                         SdCardController &sdCard,
                                         MicController &mic,
                                         SpeakerController &speaker)
  : Screen(navigator, touch, display),
    _sdCard(sdCard),
    _mic(mic),
    _speaker(speaker),
    _state(State::Idle),
    _recordedBytes(0),
    _recordStartedMs(0),
    _playStartedMs(0),
    _lastDrawMs(0),
    _lastButtonMask(0xff),
    _sdMounted(false),
    _hasRecording(false),
    _lastTouchDown(false),
    _status(F("Ready")) {
}

void VoiceRecorderScreen::onEnter() {
  refreshStorageState();
  draw();
}

void VoiceRecorderScreen::onExit() {
  if (_state == State::Recording) {
    stopRecording();
  } else if (_state == State::Playing) {
    stopPlayback(F("Stopped"));
  }
}

void VoiceRecorderScreen::handleTouch(const TouchPoint &point) {
  if (point.touched && !_lastTouchDown) {
    handlePress(point.x, point.y);
  }

  _lastTouchDown = point.touched;
}

void VoiceRecorderScreen::tick(uint32_t nowMs) {
  handleSdCardEvent(_sdCard.update());

  if (_state == State::Recording) {
    recordChunk();
  } else if (_state == State::Playing) {
    playChunk();
  }

  if (_state != State::Idle && nowMs - _lastDrawMs >= 250) {
    drawDynamicSections(false);
  }
}

void VoiceRecorderScreen::layoutButtons() {
  int16_t width = _display.width();
  int16_t buttonGap = 14;
  int16_t buttonWidth = (width - 40 - buttonGap * 2) / 3;
  if (buttonWidth > 135) {
    buttonWidth = 135;
  }
  if (buttonWidth < 80) {
    buttonWidth = 80;
  }

  int16_t totalWidth = buttonWidth * 3 + buttonGap * 2;
  int16_t startX = (width - totalWidth) / 2;

  _backButton = { 10, 10, 80, 34 };
  _recordButton = { startX, 96, buttonWidth, 54 };
  _stopButton = { (int16_t)(startX + buttonWidth + buttonGap), 96, buttonWidth, 54 };
  _playButton = { (int16_t)(startX + (buttonWidth + buttonGap) * 2), 96, buttonWidth, 54 };
}

void VoiceRecorderScreen::draw() {
  if (!_display.isReady()) {
    return;
  }

  layoutButtons();
  _lastDrawMs = millis();
  _lastStorageText = "";
  _lastStatusText = "";
  _lastDetailsText = "";
  _lastButtonMask = 0xff;

  drawStaticFrame();
  drawDynamicSections(true);
  drawButtons(true);
}

void VoiceRecorderScreen::drawStaticFrame() {
  TFT_eSPI &tft = _display.tft();
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(F("Voice Recorder"), 108, 16, 4);
}

void VoiceRecorderScreen::drawDynamicSections(bool force) {
  if (!_display.isReady()) {
    return;
  }

  _lastDrawMs = millis();
  TFT_eSPI &tft = _display.tft();
  int16_t width = _display.width();

  String storage = storageText();
  if (force || storage != _lastStorageText) {
    tft.fillRect(20, 58, width - 40, 24, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(storage, 20, 58, 2);
    _lastStorageText = storage;
  }

  if (force || _status != _lastStatusText) {
    tft.fillRect(20, 170, width - 40, 24, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString(_status, 20, 170, 2);
    _lastStatusText = _status;
  }

  String details = detailsText();
  if (force || details != _lastDetailsText) {
    tft.fillRect(20, 196, width - 40, 24, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(details, 20, 196, 2);
    _lastDetailsText = details;
  }
}

void VoiceRecorderScreen::drawButtons(bool force) {
  uint8_t mask = buttonMask();
  if (!force && mask == _lastButtonMask) {
    return;
  }

  drawButton(_backButton, F("Back"), TFT_DARKGREY, (mask & 0x01) != 0);
  drawButton(_recordButton, F("Rec"), TFT_RED, (mask & 0x02) != 0);
  drawButton(_stopButton, F("Stop"), TFT_ORANGE, (mask & 0x04) != 0);
  drawButton(_playButton, F("Play"), TFT_GREEN, (mask & 0x08) != 0);
  _lastButtonMask = mask;
}

void VoiceRecorderScreen::drawButton(const Button &button,
                                     const String &label,
                                     uint16_t color,
                                     bool enabled) {
  TFT_eSPI &tft = _display.tft();
  uint16_t fill = enabled ? color : TFT_DARKGREY;
  uint16_t text = enabled ? TFT_BLACK : TFT_LIGHTGREY;

  tft.fillRoundRect(button.x, button.y, button.w, button.h, 6, fill);
  tft.drawRoundRect(button.x, button.y, button.w, button.h, 6, TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(text, fill);
  tft.drawString(label, button.x + button.w / 2, button.y + button.h / 2, 2);
  tft.setTextDatum(TL_DATUM);
}

String VoiceRecorderScreen::storageText() {
  String storage = F("SD: ");
  storage += _sdMounted ? F("mounted") : F("waiting");
  storage += F("   Mic: ");
  storage += _mic.isPowered() ? F("on") : F("switch off");
  return storage;
}

String VoiceRecorderScreen::detailsText() {
  String details;
  if (_state == State::Recording) {
    details = F("Recording ");
    details += elapsedSeconds(millis(), _recordStartedMs);
    details += F("s, ");
    details += _recordedBytes / 1024;
    details += F(" KB");
  } else if (_state == State::Playing) {
    details = F("Playing ");
    details += elapsedSeconds(millis(), _playStartedMs);
    details += F("s");
  } else {
    details = _hasRecording ? F("Saved: /voice_last.wav") : F("No recording yet");
  }

  return details;
}

uint8_t VoiceRecorderScreen::buttonMask() {
  uint8_t mask = 0;
  bool idle = _state == State::Idle;

  if (idle) {
    mask |= 0x01;
  }
  if (idle && _sdMounted && _mic.isPowered()) {
    mask |= 0x02;
  }
  if (!idle) {
    mask |= 0x04;
  }
  if (idle && _sdMounted && _hasRecording) {
    mask |= 0x08;
  }

  return mask;
}

void VoiceRecorderScreen::handlePress(int16_t x, int16_t y) {
  if (_backButton.contains(x, y) && _state == State::Idle) {
    _navigator.pop();
    return;
  }

  if (_recordButton.contains(x, y) && _state == State::Idle) {
    startRecording();
    return;
  }

  if (_stopButton.contains(x, y) && _state == State::Recording) {
    stopRecording();
    return;
  }

  if (_stopButton.contains(x, y) && _state == State::Playing) {
    stopPlayback(F("Playback stopped"));
    return;
  }

  if (_playButton.contains(x, y) && _state == State::Idle) {
    startPlayback();
  }
}

void VoiceRecorderScreen::handleSdCardEvent(SdCardEvent event) {
  if (event == SdCardEvent::None) {
    return;
  }

  if (event == SdCardEvent::Mounted) {
    refreshStorageState();
    setStatus(F("SD mounted"));
  } else if (event == SdCardEvent::Removed) {
    if (_state == State::Recording) {
      _mic.stop();
      if (_recordingFile) {
        _recordingFile.close();
      }
      _state = State::Idle;
    } else if (_state == State::Playing) {
      stopPlayback(F("SD removed"));
    }

    _sdMounted = false;
    _hasRecording = false;
    setStatus(F("SD removed"));
  } else if (event == SdCardEvent::MountFailed) {
    _sdMounted = false;
    setStatus(F("SD mount retry failed"));
  }
}

void VoiceRecorderScreen::refreshStorageState() {
  _sdMounted = _sdCard.isMounted();
  _hasRecording = _sdMounted && SD.exists(RecordingPath);
}

void VoiceRecorderScreen::setStatus(const String &status) {
  _status = status;
  drawDynamicSections(false);
  drawButtons(false);
}

void VoiceRecorderScreen::startRecording() {
  refreshStorageState();
  _speaker.stop();
  _speaker.mute();

  if (!_sdMounted) {
    setStatus(F("Insert SD card before recording"));
    return;
  }

  if (!_mic.isReady()) {
    setStatus(_mic.lastError());
    return;
  }

  if (!_mic.isPowered()) {
    setStatus(F("Turn mic power switch on"));
    return;
  }

  if (SD.exists(RecordingPath)) {
    SD.remove(RecordingPath);
  }

  _recordingFile = SD.open(RecordingPath, FILE_WRITE);
  if (!_recordingFile) {
    setStatus(F("Could not create recording file"));
    return;
  }

  _recordedBytes = 0;
  if (!writeWavHeader(_recordingFile, _recordedBytes)) {
    _recordingFile.close();
    setStatus(F("Could not write WAV header"));
    return;
  }

  if (!_mic.start()) {
    _recordingFile.close();
    setStatus(_mic.lastError());
    return;
  }

  _state = State::Recording;
  _recordStartedMs = millis();
  _hasRecording = false;
  setStatus(F("Recording..."));
}

void VoiceRecorderScreen::stopRecording() {
  _mic.stop();

  if (_recordingFile) {
    finalizeWavHeader();
    _recordingFile.close();
  }

  _state = State::Idle;
  refreshStorageState();
  setStatus(_recordedBytes > 0 ? F("Recording saved") : F("Recording stopped empty"));
}

void VoiceRecorderScreen::recordChunk() {
  if (!_mic.isPowered()) {
    stopRecording();
    setStatus(F("Mic switch turned off"));
    return;
  }

  int16_t samples[AudioBufferSamples];
  size_t sampleCount = _mic.readPcm16(samples, AudioBufferSamples);
  if (sampleCount == 0) {
    return;
  }

  size_t bytesToWrite = sampleCount * sizeof(samples[0]);
  size_t written = _recordingFile.write((uint8_t *)samples, bytesToWrite);
  if (written != bytesToWrite) {
    stopRecording();
    setStatus(F("SD write failed"));
    return;
  }

  _recordedBytes += written;

  if ((_recordedBytes % 8192) == 0) {
    _recordingFile.flush();
  }
}

void VoiceRecorderScreen::startPlayback() {
  refreshStorageState();

  if (!_sdMounted || !_hasRecording) {
    setStatus(F("No recording to play"));
    return;
  }

  uint32_t sampleRate = 0;
  if (!openWavForPlayback(sampleRate)) {
    setStatus(F("Could not open WAV recording"));
    return;
  }

  if (!_speaker.isReady() && !_speaker.begin()) {
    _playbackFile.close();
    setStatus(_speaker.lastError());
    return;
  }

  if (!_speaker.setSampleRate(sampleRate)) {
    _playbackFile.close();
    setStatus(_speaker.lastError());
    return;
  }

  int16_t silence[AudioBufferSamples];
  memset(silence, 0, sizeof(silence));
  _speaker.writePcm16(silence, AudioBufferSamples, 20);
  _speaker.unmute();

  _state = State::Playing;
  _playStartedMs = millis();
  setStatus(F("Playing..."));
}

void VoiceRecorderScreen::stopPlayback(const String &status) {
  if (_playbackFile) {
    _playbackFile.close();
  }

  _speaker.stop();
  _state = State::Idle;
  setStatus(status);
}

void VoiceRecorderScreen::playChunk() {
  int16_t samples[AudioBufferSamples];
  int bytesRead = _playbackFile.read((uint8_t *)samples, sizeof(samples));
  if (bytesRead <= 0) {
    stopPlayback(F("Playback finished"));
    return;
  }

  _speaker.writePcm16(samples, bytesRead / sizeof(samples[0]));
}

bool VoiceRecorderScreen::writeWavHeader(fs::File &file, uint32_t dataBytes) {
  uint8_t header[44];
  memset(header, 0, sizeof(header));

  memcpy(header + 0, "RIFF", 4);
  putLe32(header + 4, 36 + dataBytes);
  memcpy(header + 8, "WAVE", 4);
  memcpy(header + 12, "fmt ", 4);
  putLe32(header + 16, 16);
  putLe16(header + 20, 1);
  putLe16(header + 22, 1);
  putLe32(header + 24, _mic.sampleRate());
  putLe32(header + 28, _mic.sampleRate() * 2);
  putLe16(header + 32, 2);
  putLe16(header + 34, 16);
  memcpy(header + 36, "data", 4);
  putLe32(header + 40, dataBytes);

  return file.write(header, sizeof(header)) == sizeof(header);
}

bool VoiceRecorderScreen::finalizeWavHeader() {
  if (!_recordingFile) {
    return false;
  }

  if (!_recordingFile.seek(0)) {
    return false;
  }

  bool ok = writeWavHeader(_recordingFile, _recordedBytes);
  _recordingFile.flush();
  return ok;
}

bool VoiceRecorderScreen::openWavForPlayback(uint32_t &sampleRate) {
  _playbackFile = SD.open(RecordingPath, FILE_READ);
  if (!_playbackFile) {
    return false;
  }

  uint8_t header[44];
  if (_playbackFile.read(header, sizeof(header)) != sizeof(header)) {
    _playbackFile.close();
    return false;
  }

  bool valid = memcmp(header + 0, "RIFF", 4) == 0
    && memcmp(header + 8, "WAVE", 4) == 0
    && memcmp(header + 12, "fmt ", 4) == 0
    && memcmp(header + 36, "data", 4) == 0
    && readLe16(header + 20) == 1
    && readLe16(header + 22) == 1
    && readLe16(header + 34) == 16
    && readLe32(header + 40) > 0;

  if (!valid) {
    _playbackFile.close();
    return false;
  }

  sampleRate = readLe32(header + 24);
  return true;
}

uint32_t VoiceRecorderScreen::elapsedSeconds(uint32_t nowMs, uint32_t startedMs) const {
  if (startedMs == 0 || nowMs < startedMs) {
    return 0;
  }

  return (nowMs - startedMs) / 1000;
}
