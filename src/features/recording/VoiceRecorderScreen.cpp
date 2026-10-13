#include "VoiceRecorderScreen.h"

#include "../../app/Navigator.h"
#include "../../services/audio/WavFormat.h"
#include "../../ui/UiKit.h"

#include <SD.h>
#include <string.h>

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
    _lastMicPowered(true),
    _lastTouchDown(false),
    _status(F("Ready")) {
}

void VoiceRecorderScreen::onEnter() {
  _lastMicPowered = _mic.isPowered();

  // Paint the recorder controls before probing the card so navigation never
  // waits on an SD response just to show this screen.
  draw();
  yield();
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
    if (UiKit::navigationIconContains(point.x, point.y) && _state == State::Idle) {
      _navigator.pop();
    } else {
    handlePress(point.x, point.y);
    }
  }

  _lastTouchDown = point.touched;
}

void VoiceRecorderScreen::tick(uint32_t nowMs) {
  handleSdCardEvent(_sdCard.takeEvent());

  bool micPowered = _mic.isPowered();
  if (micPowered != _lastMicPowered) {
    _lastMicPowered = micPowered;
    if (_state == State::Idle) {
      setStatus(micPowered ? F("Mic switch turned on") : F("Mic switch turned off"));
    }
  }

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
  _display.tft().fillScreen(TFT_BLACK);
  UiKit::drawTopAppBar(_display.tft(), _display.width(), F("Voice Recorder"));
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
    UiText::draw(tft, storage, 20, 58, TFT_LIGHTGREY, TFT_BLACK, UiText::Size::Compact);
    _lastStorageText = storage;
  }

  if (force || _status != _lastStatusText) {
    tft.fillRect(20, 170, width - 40, 24, TFT_BLACK);
    UiText::draw(tft, _status, 20, 170, TFT_CYAN, TFT_BLACK, UiText::Size::Compact);
    _lastStatusText = _status;
  }

  String details = detailsText();
  if (force || details != _lastDetailsText) {
    tft.fillRect(20, 196, width - 40, 24, TFT_BLACK);
    UiText::draw(tft, details, 20, 196, TFT_LIGHTGREY, TFT_BLACK, UiText::Size::Compact);
    _lastDetailsText = details;
  }
}

void VoiceRecorderScreen::drawButtons(bool force) {
  uint8_t mask = buttonMask();
  if (!force && mask == _lastButtonMask) {
    return;
  }

  drawButton(_recordButton, F("Rec"), TFT_RED, (mask & 0x01) != 0);
  drawButton(_stopButton, F("Stop"), TFT_ORANGE, (mask & 0x02) != 0);
  drawButton(_playButton, F("Play"), TFT_GREEN, (mask & 0x04) != 0);
  _lastButtonMask = mask;
}

void VoiceRecorderScreen::drawButton(const Button &button,
                                     const String &label,
                                     uint16_t color,
                                     bool enabled) {
  TFT_eSPI &tft = _display.tft();
  uint16_t fill = enabled ? color : TFT_DARKGREY;
  uint16_t text = enabled ? TFT_BLACK : TFT_LIGHTGREY;

  tft.fillSmoothRoundRect(button.x, button.y, button.w, button.h, 6, fill, TFT_BLACK);
  UiText::draw(tft, label, button.x + button.w / 2, button.y + button.h / 2,
               text, fill, UiText::Size::Content, MC_DATUM);
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

  if (idle && _sdMounted && _mic.isPowered()) {
    mask |= 0x01;
  }
  if (!idle) {
    mask |= 0x02;
  }
  if (idle && _sdMounted && _hasRecording) {
    mask |= 0x04;
  }

  return mask;
}

void VoiceRecorderScreen::handlePress(int16_t x, int16_t y) {
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

  if (!_sdMounted) {
    setStatus(F("Insert SD card before recording"));
    return;
  }

  if (!_mic.isReady() && !_mic.begin()) {
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

  if (!_speaker.start(sampleRate)) {
    _playbackFile.close();
    setStatus(_speaker.lastError());
    return;
  }

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
  uint8_t header[WavFormat::HeaderBytes];
  WavFormat::makeHeader(header, _mic.sampleRate(), dataBytes);
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

  uint8_t header[WavFormat::HeaderBytes];
  uint32_t dataBytes = 0;
  if (_playbackFile.read(header, sizeof(header)) != sizeof(header)
      || !WavFormat::parseHeader(header, sampleRate, dataBytes)) {
    _playbackFile.close();
    return false;
  }

  return true;
}

uint32_t VoiceRecorderScreen::elapsedSeconds(uint32_t nowMs, uint32_t startedMs) const {
  if (startedMs == 0 || nowMs < startedMs) {
    return 0;
  }

  return (nowMs - startedMs) / 1000;
}
