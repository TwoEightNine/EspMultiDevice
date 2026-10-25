#pragma once

#include "../../core/Screen.h"

class AboutScreen : public Screen {
public:
  AboutScreen(Navigator &navigator,
              TouchController &touch,
              DisplayController &display);

  void onEnter() override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  // Vertical space between the repository text lines and the logo.
  static const int16_t LogoGap = 8;

  bool _lastTouchDown;

  void draw();
};
