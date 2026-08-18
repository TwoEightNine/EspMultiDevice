#pragma once

#include <Arduino.h>

#include "DisplayController.h"
#include "TouchController.h"

class Navigator;

class Screen {
public:
  Screen(Navigator &navigator, TouchController &touch, DisplayController &display)
    : _navigator(navigator),
      _touch(touch),
      _display(display) {
  }

  virtual ~Screen() = default;

  virtual void onEnter() {
  }

  virtual void onExit() {
  }

  virtual void handleTouch(const TouchPoint &point) {
    (void)point;
  }

  virtual void tick(uint32_t nowMs) {
    (void)nowMs;
  }

protected:
  Navigator &_navigator;
  TouchController &_touch;
  DisplayController &_display;
};
