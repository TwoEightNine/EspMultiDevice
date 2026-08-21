#pragma once

#include <Arduino.h>

#include "Screen.h"

class SettingsScreen : public Screen {
public:
  SettingsScreen(Navigator &navigator,
                 TouchController &touch,
                 DisplayController &display);

  void onEnter() override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  static const int16_t ListPadding = 16;
  static const int16_t RowTop = 88;
  static const int16_t RowHeight = 30;
  static const uint8_t OptionCount = 3;

  bool _lastTouchDown;

  void draw();
  void drawTopBar();
  void drawOptions();
};
