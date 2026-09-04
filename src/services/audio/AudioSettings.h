#pragma once

#include <Arduino.h>

class SpeakerController;

// Stores the user's global output-volume preference and keeps the shared
// speaker output in sync with it.
class AudioSettings {
public:
  static const uint8_t MinVolume = 0;
  static const uint8_t MaxVolume = 10;
  static const uint8_t DefaultVolume = 5;

  explicit AudioSettings(SpeakerController &speaker);

  void begin();
  uint8_t volume() const;
  bool setVolume(uint8_t volume);

private:
  SpeakerController &_speaker;
  uint8_t _volume;
};
