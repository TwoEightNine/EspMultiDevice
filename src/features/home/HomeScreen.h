#pragma once

#include <Arduino.h>

#include "../../core/BatteryController.h"
#include "../../services/storage/SdCardController.h"
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
             BatteryController &battery);

  void onEnter() override;
  void onResult(const ScreenResult &result) override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  static const uint8_t AppCount = 6;
  static const uint8_t AppColumns = 3;
  static const uint8_t FilesAppIndex = 2;
  static const int16_t StatusBarHeight = 24;
  static const int16_t IconDiameter = 80;
  static const uint32_t BatteryRefreshIntervalMs = 5000UL;

  enum class AppAction : uint8_t {
    AiAssistant,
    Files,
    Recorder,
    Settings,
    Browser,
    Workout
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
  BatteryController &_battery;
  bool _sdMounted;
  bool _wifiConnected;
  float _batteryVoltage;
  uint8_t _batteryPercent;
  bool _lastTouchDown;
  bool _filesPressPending;
  bool _filesHoldTriggered;
  uint32_t _filesPressedMs;
  uint32_t _lastClockCheckMs;
  uint32_t _lastBatteryCheckMs;
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
  uint16_t batteryFillColor(uint8_t percentage) const;
  String batteryVoltageText(float voltage) const;
  String clockText(const struct tm &localTime) const;
  String truncateLabel(const String &label, int16_t maxWidth);
  void setSdState(bool mounted);
  void setWifiState(bool connected);
  void setBatteryReading(const BatteryReading &reading);
  void refreshSdState();
  void handleSdCardEvent(SdCardEvent event);
};
