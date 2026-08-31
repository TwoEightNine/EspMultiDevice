#pragma once

#include <Arduino.h>

#include "SdCardController.h"
#include "Screen.h"
#include "TimeController.h"
#include "WiFiController.h"

class HomeScreen : public Screen {
public:
  HomeScreen(Navigator &navigator,
             TouchController &touch,
             DisplayController &display,
             SdCardController &sdCard,
             WiFiController &wifi,
             TimeController &time);

  void onEnter() override;
  void onResult(const ScreenResult &result) override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  static const uint8_t AppCount = 4;
  static const uint8_t AppColumns = 3;
  static const int16_t StatusBarHeight = 24;
  static const int16_t IconDiameter = 80;
  static const int16_t IconBitmapSize = 64;
  static const uint8_t BatteryStubPercent = 78;

  enum class AppAction : uint8_t {
    Files,
    Recorder,
    Settings,
    Browser
  };

  struct AppTile {
    const char *label;
    AppAction action;
    uint16_t circleColor;
    // Replace nullptr with a 64x64, 1-bit bitmap when the final asset is ready.
    // TFT_eSPI renders these with a runtime foreground color, keeping icons monochrome and themeable.
    const uint8_t *bitmap;
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
  };

  SdCardController &_sdCard;
  WiFiController &_wifi;
  TimeController &_time;
  bool _sdMounted;
  bool _wifiConnected;
  uint8_t _batteryPercent;
  bool _lastTouchDown;
  bool _filesPressPending;
  bool _filesHoldTriggered;
  uint32_t _filesPressedMs;
  uint32_t _lastClockCheckMs;
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
  void refreshSdState();
  void handleSdCardEvent(SdCardEvent event);
};
