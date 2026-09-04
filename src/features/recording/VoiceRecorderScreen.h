#pragma once

#include <Arduino.h>
#include <FS.h>

#include "../../services/audio/MicController.h"
#include "../../core/Screen.h"
#include "../../services/storage/SdCardController.h"
#include "../../services/audio/SpeakerController.h"

class VoiceRecorderScreen : public Screen {
public:
  VoiceRecorderScreen(Navigator &navigator,
                      TouchController &touch,
                      DisplayController &display,
                      SdCardController &sdCard,
                      MicController &mic,
                      SpeakerController &speaker);

  void onEnter() override;
  void onExit() override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  enum class State : uint8_t {
    Idle,
    Recording,
    Playing
  };

  struct Button {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;

    bool contains(int16_t px, int16_t py) const {
      return px >= x && px < x + w && py >= y && py < y + h;
    }
  };

  static const char *RecordingPath;
  static const size_t AudioBufferSamples = 256;

  SdCardController &_sdCard;
  MicController &_mic;
  SpeakerController &_speaker;
  State _state;
  fs::File _recordingFile;
  fs::File _playbackFile;
  uint32_t _recordedBytes;
  uint32_t _recordStartedMs;
  uint32_t _playStartedMs;
  uint32_t _lastDrawMs;
  uint8_t _lastButtonMask;
  bool _sdMounted;
  bool _hasRecording;
  bool _lastMicPowered;
  bool _lastTouchDown;
  String _status;
  String _lastStorageText;
  String _lastStatusText;
  String _lastDetailsText;
  Button _recordButton;
  Button _stopButton;
  Button _playButton;

  void layoutButtons();
  void draw();
  void drawStaticFrame();
  void drawDynamicSections(bool force);
  void drawButtons(bool force);
  void drawButton(const Button &button, const String &label, uint16_t color, bool enabled);
  String storageText();
  String detailsText();
  uint8_t buttonMask();
  void handlePress(int16_t x, int16_t y);
  void handleSdCardEvent(SdCardEvent event);
  void refreshStorageState();
  void setStatus(const String &status);
  void startRecording();
  void stopRecording();
  void recordChunk();
  void startPlayback();
  void stopPlayback(const String &status);
  void playChunk();
  bool writeWavHeader(fs::File &file, uint32_t dataBytes);
  bool finalizeWavHeader();
  bool openWavForPlayback(uint32_t &sampleRate);
  uint32_t elapsedSeconds(uint32_t nowMs, uint32_t startedMs) const;
};
