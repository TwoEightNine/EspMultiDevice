#pragma once

#include <Arduino.h>

#include "../../core/Screen.h"
#include "../../ui/UiKit.h"

class SettingsScreen : public Screen {
public:
  SettingsScreen(Navigator &navigator,
                 TouchController &touch,
                 DisplayController &display);

  void onEnter() override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  static const int16_t RowTop = UiKit::AppBarHeight + 16;
  static const int16_t RowHeight = UiKit::ListCellHeight;
  static const uint8_t OptionCount = 6;

  bool _lastTouchDown;

  void draw();
  void drawTopBar();
  void drawOptions();
};
