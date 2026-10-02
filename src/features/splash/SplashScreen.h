#pragma once


#include <Arduino.h>

#include "../../core/Screen.h"

class WiFiController;

class SplashScreen : public Screen {
public:
  SplashScreen(Navigator &navigator,
               TouchController &touch,
               DisplayController &display,
               WiFiController &wifi);

  void onEnter() override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  uint32_t _enteredMs = 0;
  bool _transitioned = false;
  WiFiController &_wifi;
};
