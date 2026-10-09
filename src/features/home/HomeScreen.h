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
  void onResume() override;
  void onExit() override;
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
  static constexpr int16_t PageContentBottom = 416;
  static constexpr int16_t PageContentHeight = PageContentBottom - StatusBarHeight;
  static constexpr int16_t PageFrameBottom = 461;
  static constexpr int16_t PageFrameHeight = PageFrameBottom - StatusBarHeight;
  static constexpr int16_t IconDiameter = 80;
  static constexpr int16_t SwipeThreshold = 48;
  // The calibrated resistive panel is least stable near its left edge. Keep
  // taps tolerant there while the separate 48px threshold still decides
  // whether a page changes.
  static constexpr int16_t TapSlop = 24;
  static constexpr int16_t FlickVelocity = 650;
  static constexpr uint16_t VelocityWindowMs = 100;
  static constexpr uint16_t AnimationFrameMs = 16;
  static constexpr uint16_t SettleDurationMs = 180;

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
  AppCatalogLoader::Catalog _pendingCatalog;
  TFT_eSprite _pageStrip;
  TFT_eSprite _pageFrame;
  uint32_t _catalogRevision;
  bool _sdMounted;
  bool _wifiConnected;
  float _batteryVoltage;
  bool _gestureActive;
  bool _gestureCancelled;
  bool _horizontalDrag;
  bool _filesHoldTriggered;
  bool _settling;
  bool _pageStripReady;
  int8_t _pressedIndex;
  uint16_t _gestureStartX;
  uint16_t _gestureStartY;
  uint32_t _gestureStartedMs;
  int16_t _velocityStartX;
  int16_t _velocityEndX;
  uint32_t _velocityStartMs;
  uint32_t _velocityEndMs;
  int32_t _gestureBaseScroll;
  int32_t _visibleScroll;
  int32_t _renderedScroll;
  int32_t _settleFromScroll;
  int32_t _settleToScroll;
  uint32_t _settleStartedMs;
  uint16_t _settleDurationMs;
  uint32_t _lastAnimationFrameMs;
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
  template <typename Canvas>
  void drawAppTileTo(Canvas &canvas, const AppTile &app,
                     int16_t xOffset, int16_t yOffset);
  void drawPageIndicator();
  void drawPageIndicatorTo(TFT_eSPI &target, int16_t yOffset);
  template <typename Canvas>
  void drawAppIconTo(Canvas &canvas, const AppTile &app,
                     int16_t centerX, int16_t centerY);
  bool buildPageStrip();
  void destroyPageStrip();
  void restoreCachedPage();
  void renderPageViewport(bool force = false);
  void updateCatalogTiles();
  void redrawStripSlot(uint8_t index);
  void presentSlot(uint8_t index);
  void presentPageIndicator();
  void slotBounds(uint8_t index, int16_t &x, int16_t &y,
                  int16_t &width, int16_t &height) const;
  uint64_t tileSignature(const AppTile &app) const;
  uint8_t pageCount() const;
  int8_t appAt(uint16_t x, uint16_t y) const;
  void activateApp(const AppTile &app);
  void updateGesture(uint16_t x, uint16_t y);
  void finishGesture(uint32_t nowMs);
  void beginSettle(uint8_t targetPage, uint32_t nowMs);
  void updateSettle(uint32_t nowMs, bool force = false);
  int16_t releaseVelocity(uint32_t nowMs) const;
  void cancelMotion();
  void resetGesture();
  String clockText(const struct tm &localTime) const;
  String truncateLabel(const String &label, int16_t maxWidth);
  void setSdState(bool mounted);
  void setWifiState(bool connected);
  void setBatteryVoltage(float voltage);
  void refreshSdState();
  void handleSdCardEvent(SdCardEvent event);
};
