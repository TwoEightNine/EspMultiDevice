#pragma once

#include <Arduino.h>

#include "../../core/Screen.h"
#include "../../services/storage/UsbMassStorageController.h"

class UsbDriveScreen : public Screen {
public:
  UsbDriveScreen(Navigator &navigator,
                 TouchController &touch,
                 DisplayController &display,
                 UsbMassStorageController *usbStorage,
                 bool transferBoot);

  void onEnter() override;
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

  static const uint32_t RestartDelayMs = 750;

  UsbMassStorageController *_usbStorage;
  bool _transferBoot;
  bool _serviceStarted;
  bool _lastTouchDown;
  bool _restartPending;
  uint32_t _restartAtMs;
  UsbDriveState _lastState;
  String _message;
  Rect _primaryButton;

  void draw();
  void drawIntro();
  void drawTransferState(UsbDriveState state);
  void requestTransferBoot();
  void scheduleRestart(uint32_t nowMs);
  void restartNow();
};
