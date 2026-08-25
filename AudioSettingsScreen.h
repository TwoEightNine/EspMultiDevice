#pragma once

#include "AudioSettings.h"
#include "Screen.h"

class AudioSettingsScreen : public Screen {
public:
  AudioSettingsScreen(Navigator &navigator,
                      TouchController &touch,
                      DisplayController &display,
                      AudioSettings &audioSettings);

  void onEnter() override;
  void handleTouch(const TouchPoint &point) override;

private:
  static const uint8_t Columns = 6;
  static const uint8_t LevelCount = AudioSettings::MaxVolume + 1;
  static const int16_t GridLeft = 16;
  static const int16_t GridTop = 126;
  static const int16_t GridGap = 8;
  static const int16_t GridHeight = 48;

  AudioSettings &_audioSettings;
  bool _lastTouchDown;

  void draw();
  void drawLevels();
  int8_t levelAt(int16_t x, int16_t y) const;
};
