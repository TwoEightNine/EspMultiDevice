#pragma once

#include "../../core/Screen.h"
#include "AppRuntime.h"

class BatteryHistoryService;
class SdCardController;
class WiFiController;
class SpeakerController;
class AudioSettings;

class AppHostScreen : public Screen {
public:
  AppHostScreen(Navigator &navigator, TouchController &touch,
                DisplayController &display, SdCardController &card,
                WiFiController &wifi, BatteryHistoryService &batteryHistory,
                SpeakerController &speaker, AudioSettings &audioSettings);
  void onEnter(const ScreenData &data) override;
  void onExit() override;
  bool releaseResources() override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  static constexpr int16_t RowTop = 56;
  SdCardController &_card;
  WiFiController &_wifi;
  BatteryHistoryService &_batteryHistory;
  AppRuntime _runtime;
  AppRuntime::Scene _scene{};
  AppRuntime::Frame _frame{};
  uint32_t _revision = 0;
  uint32_t _frameRevision = 0;
  uint32_t _lastStatusMs = 0;
  bool _lastTouchDown = false;
  bool _closeRequested = false;
  bool _running = false;
  bool _canvasMode = false;

  AppRuntime::Status statusSnapshot();
  void publishStatus();
  void draw();
  void drawFrame();
};
