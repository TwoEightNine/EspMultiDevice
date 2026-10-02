#pragma once

#include <Arduino.h>

#include "../../services/storage/SdCardController.h"
#include "../../services/storage/BatteryHistoryService.h"
#include "../../core/Screen.h"
#include "../../services/network/TimeController.h"
#include "../../services/network/WiFiController.h"

class HomeScreen : public Screen {
public:
  HomeScreen(Navigator &navigator,
             TouchController &touch,
             DisplayController &display,
             SdCardController &sdCard,
             WiFiController &wifi,
             TimeController &time,
             BatteryHistoryService &batteryHistory);

  void onEnter() override;
  void onResult(const ScreenResult &result) override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  static const uint8_t AppCount = 8;
  static const uint8_t AppColumns = 3;
  static const uint8_t FilesAppIndex = 2;
  static const int16_t StatusBarHeight = 24;
  static const int16_t IconDiameter = 80;

  enum class AppAction : uint8_t {
    AiAssistant,
    Files,
    Recorder,
    Settings,
    Browser,
    UsbDrive,
    Workout,
    Apps
  };

  struct AppTile {
    const char *label;
    AppAction action;
    uint16_t circleColor;
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
  };

  SdCardController &_sdCard;
  WiFiController &_wifi;
  TimeController &_time;
  BatteryHistoryService &_batteryHistory;
  bool _sdMounted;
  bool _wifiConnected;
  float _batteryVoltage;
  bool _lastTouchDown;
  bool _filesPressPending;
  bool _filesHoldTriggered;
  uint32_t _filesPressedMs;
  uint32_t _lastClockCheckMs;
  uint32_t _lastBatteryGeneration;
  String _lastClockText;
  AppTile _apps[AppCount];

  void draw();
  void layoutApps();
  void drawStatusBar();
  void drawClock(bool force);
  void drawSdIcon();
  void drawWifiIcon();
  void drawBattery();
  void drawApps();
  void drawAppIcon(const AppTile &app, int16_t centerX, int16_t centerY);
  String clockText(const struct tm &localTime) const;
  String truncateLabel(const String &label, int16_t maxWidth);
  void setSdState(bool mounted);
  void setWifiState(bool connected);
  void setBatteryVoltage(float voltage);
  void refreshSdState();
  void handleSdCardEvent(SdCardEvent event);
};
