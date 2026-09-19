#pragma once

#include "../../core/Screen.h"
#include "../../services/led/LedSettings.h"
#include "../../ui/UiKit.h"

class LedScreen : public Screen {
public:
  LedScreen(Navigator &navigator,
            TouchController &touch,
            DisplayController &display,
            LedSettings &ledSettings);

  void onEnter() override;
  void handleTouch(const TouchPoint &point) override;

private:
  struct Rect {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
  };

  static const uint8_t PaletteColumns = 3;
  static const int16_t EnabledRowY = UiKit::AppBarHeight;

  LedSettings &_ledSettings;
  bool _lastTouchDown;
  Rect _toggleBounds;
  Rect _swatchBounds[LedSettings::ColorCount];
  String _error;

  void draw();
  void drawEnabledRow();
  void drawPalette();
  bool contains(const Rect &bounds, int16_t x, int16_t y) const;
};
