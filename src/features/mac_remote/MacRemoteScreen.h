#pragma once

#include <Arduino.h>

#include "../../core/Screen.h"
#include "../../services/usb/MacRemoteController.h"

class MacRemoteScreen : public Screen {
public:
  MacRemoteScreen(Navigator &navigator,
                  TouchController &touch,
                  DisplayController &display,
                  MacRemoteController &controller);

  void onEnter() override;
  void onExit() override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  struct Rect {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;

    bool contains(int16_t touchX, int16_t touchY) const {
      return touchX >= x && touchX < x + w &&
             touchY >= y && touchY < y + h;
    }
  };

  static const uint8_t ActionCount = 9;
  static const uint8_t ColumnCount = 3;
  static const uint8_t NoAction = 0xff;

  struct ActionCell {
    const char *label;
    MacRemoteAction action;
    Rect bounds;
  };

  MacRemoteController &_controller;
  ActionCell _actions[ActionCount];
  MacRemoteState _lastState;
  bool _lastTouchDown;
  bool _disconnected;
  bool _lastBusy;
  uint8_t _pressedAction;
  String _message;
  uint32_t _messageUntilMs;

  void layoutActions();
  void draw();
  void drawStatus();
  void drawActions();
  void drawAction(uint8_t index);
  void clearMessageIfExpired(uint32_t nowMs);
};
