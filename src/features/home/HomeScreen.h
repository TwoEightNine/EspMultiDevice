#pragma once

#include <Arduino.h>

#include "../../core/Screen.h"
#include "../../features/apps/AppCatalogLoader.h"
#include "../../services/network/TimeController.h"
#include "../../services/network/WiFiController.h"
#include "../../services/storage/BatteryHistoryService.h"
#include "../../services/storage/SdCardController.h"

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
  bool releaseResources() override;

private:
  static constexpr uint8_t BuiltInAppCount = 8;
  static constexpr uint8_t AppColumns = 3;
  static constexpr uint8_t PageSize = 9;
  static constexpr uint8_t MaxHomeApps = BuiltInAppCount + AppCatalogLoader::MaxApps;
  static constexpr int16_t StatusBarHeight = 24;
  static constexpr int16_t IconDiameter = 80;
  static constexpr int16_t SwipeThreshold = 48;
  static constexpr int16_t TapSlop = 12;

  enum class AppAction : uint8_t {
    AiAssistant,
    Files,
    Recorder,
    Settings,
    Browser,
    UsbDrive,
    Workout,
    MacRemote,
    External
  };

  struct AppTile {
    char label[41];
    char externalId[33];
    AppAction action;
    uint16_t circleColor;
    const uint16_t *iconColors;
    const uint8_t *iconAlpha;
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
  };

  SdCardController &_sdCard;
  WiFiController &_wifi;
  TimeController &_time;
  BatteryHistoryService &_batteryHistory;
  AppCatalogLoader _catalogLoader;
  AppCatalogLoader::Catalog _catalog;
  uint32_t _catalogRevision;
  bool _sdMounted;
  bool _wifiConnected;
  float _batteryVoltage;
  bool _gestureActive;
  bool _gestureCancelled;
  bool _swipeHandled;
  bool _filesHoldTriggered;
  int8_t _pressedIndex;
  uint16_t _gestureStartX;
  uint16_t _gestureStartY;
  uint32_t _gestureStartedMs;
  uint32_t _lastClockCheckMs;
  uint32_t _lastBatteryGeneration;
  String _lastClockText;
  uint8_t _appCount;
  uint8_t _page;
  AppTile _apps[MaxHomeApps];

  void draw();
  void layoutApps();
  void rebuildApps();
  void sortApps();
  void drawStatusBar();
  void drawClock(bool force);
  void drawSdIcon();
  void drawWifiIcon();
  void drawBattery();
  void drawApps();
  void drawPageIndicator();
  void drawAppIcon(const AppTile &app, int16_t centerX, int16_t centerY);
  int8_t appAt(uint16_t x, uint16_t y) const;
  void activateApp(const AppTile &app);
  void updateGesture(uint16_t x, uint16_t y);
  void resetGesture();
  String clockText(const struct tm &localTime) const;
  String truncateLabel(const String &label, int16_t maxWidth);
  void setSdState(bool mounted);
  void setWifiState(bool connected);
  void setBatteryVoltage(float voltage);
  void refreshSdState();
  void handleSdCardEvent(SdCardEvent event);
};
