#pragma once

#include <Arduino.h>

#include "DisplayController.h"
#include "Navigation.h"
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

  virtual void onEnter(const ScreenData &data) {
    (void)data;
    onEnter();
  }

  virtual void onExit() {
  }

  // Called when this screen becomes active again after the screen above it
  // is closed. The default preserves the existing behavior for screens that
  // do not need to distinguish a fresh entry from a return.
  virtual void onResume() {
    onEnter();
  }

  virtual void onResult(const ScreenResult &result) {
    (void)result;
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
