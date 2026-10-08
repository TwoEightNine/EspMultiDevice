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

  // Called only when this stack entry is being permanently removed, not when
  // another screen temporarily covers it. Release screen-scoped buffers,
  // sockets, codecs, queues, and task stacks here. Returning false defers the
  // navigation so asynchronous workers can reach a safe stop in tick().
  virtual bool releaseResources() {
    return true;
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

  // Called for a screen that remains on Navigator's stack underneath the
  // active screen. Most screens have no suspended work. A screen that owns a
  // persistent background service may override this, but it must not draw,
  // handle touch, consume shared one-recipient events, or navigate.
  virtual void tickSuspended(uint32_t nowMs) {
    (void)nowMs;
  }

protected:
  Navigator &_navigator;
  TouchController &_touch;
  DisplayController &_display;
};
