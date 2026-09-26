#pragma once

#include "../../core/Screen.h"
#include "AppRuntime.h"

class BatteryController;
class SdCardController;
class WiFiController;

class AppHostScreen : public Screen {
public:
  AppHostScreen(Navigator &navigator, TouchController &touch,
                DisplayController &display, SdCardController &card,
                WiFiController &wifi, BatteryController &battery);
  void onEnter(const ScreenData &data) override;
  void onExit() override;
  bool releaseResources() override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  static constexpr int16_t RowTop = 56;
  SdCardController &_card;
  WiFiController &_wifi;
  BatteryController &_battery;
  AppRuntime _runtime;
  AppRuntime::Scene _scene{};
  uint32_t _revision = 0;
  uint32_t _lastStatusMs = 0;
  bool _lastTouchDown = false;
  bool _closeRequested = false;
  bool _running = false;

  AppRuntime::Status statusSnapshot();
  void publishStatus();
  void draw();
};
