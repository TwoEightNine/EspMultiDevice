#pragma once

#include "../../core/Screen.h"
#include "AppRuntime.h"

class BatteryHistoryService;
class SdCardController;
class WiFiController;
class SpeakerController;
class AudioSettings;
class MicController;
class MediaCache;

class AppHostScreen : public Screen {
public:
  AppHostScreen(Navigator &navigator, TouchController &touch,
                DisplayController &display, SdCardController &card,
                WiFiController &wifi, BatteryHistoryService &batteryHistory,
                SpeakerController &speaker, AudioSettings &audioSettings,
                MicController &mic, MediaCache &mediaCache);
  void onEnter(const ScreenData &data) override;
  void onExit() override;
  bool releaseResources() override;
  void onResume() override;
  void onResult(const ScreenResult &result) override;
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
  // SDK 4: a viewer or keyboard this screen pushed for the paused app.
  AppRuntime::Navigation _navigation{};
  String _promptText;
  bool _covering = false;
  bool _promptPending = false;
  bool _promptAnswered = false;
  bool _resumePending = false;
  // Canvas touch tracked from touch-down to release for `on_touch_end`.
  bool _touchActive = false;
  uint32_t _touchStartedMs = 0;
  uint16_t _touchX = 0;
  uint16_t _touchY = 0;

  AppRuntime::Status statusSnapshot();
  void publishStatus();
  void redraw();
  void openNavigation();
  void draw();
  void drawFrame();
};
