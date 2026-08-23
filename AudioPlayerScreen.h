#pragma once

#include <Arduino.h>
#include <FS.h>

#include "Screen.h"
#include "SdCardController.h"
#include "SpeakerController.h"

class AudioPlayerScreen : public Screen {
public:
  AudioPlayerScreen(Navigator &navigator,
                    TouchController &touch,
                    DisplayController &display,
                    SdCardController &sdCard,
                    SpeakerController &speaker);

  void onEnter(const ScreenData &data) override;
  void onExit() override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  enum class State : uint8_t {
    Ready,
    Playing,
    Paused,
    Finished,
    Error
  };

  static const uint16_t SourceBufferBytes = 4096;
  static const uint16_t OutputFrames = 256;
  static const int16_t BackTouchWidth = 80;

  SdCardController &_sdCard;
  SpeakerController &_speaker;
  fs::File _file;
  String _path;
  String _fileName;
  String _error;
  State _state;
  bool _sdMounted;
  bool _lastTouchDown;
  uint32_t _lastDrawMs;
  uint32_t _finishAtMs;
  uint32_t _dataOffset;
  uint32_t _dataBytes;
  uint32_t _frameCount;
  uint32_t _framePosition;
  uint32_t _sampleRate;
  uint16_t _channels;
  uint16_t _bitsPerSample;
  uint16_t _blockAlign;
  uint8_t _sourceBuffer[SourceBufferBytes];
  int16_t _outputBuffer[OutputFrames];

  void reset();
  bool loadSource(const String &path);
  bool parseWav();
  bool parseFormatChunk(uint32_t offset, uint32_t size);
  bool fail(const __FlashStringHelper *message);
  bool readExact(uint8_t *buffer, size_t length);
  bool seekTo(uint32_t position);
  bool startPlayback();
  void pausePlayback();
  void finishPlayback();
  void stopSpeaker();
  void streamChunk();
  int32_t decodeSample(const uint8_t *sample) const;
  void handleSdCardEvent(SdCardEvent event);

  void draw(bool force = true);
  void drawTopBar();
  void drawDynamic(bool force);
  void drawPlayButton();
  void drawProgress();
  void drawError();
  String displayName(const String &path) const;
  String truncateLabel(const String &label, int16_t maxWidth);
  String timeText(uint32_t frames) const;
  bool controlContains(int16_t x, int16_t y) const;
};
