#pragma once

#include "../../core/Screen.h"
#include "../../services/network/WiFiController.h"
#include "../../services/storage/SdCardController.h"
#include "SdSpeedTestWorker.h"
#include "WiFiSpeedTestWorker.h"

class WiFiSpeedTestScreen : public Screen {
public:
  WiFiSpeedTestScreen(Navigator &navigator,
                      TouchController &touch,
                      DisplayController &display,
                      WiFiController &wifi,
                      SdCardController &sdCard);

  void onEnter() override;
  bool releaseResources() override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  enum class Mode : uint8_t { WiFi, Sd };

  struct Rect {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
  };

  WiFiController &_wifi;
  SdCardController &_sdCard;
  WiFiSpeedTestWorker _wifiWorker;
  SdSpeedTestWorker _sdWorker;
  WiFiSpeedTestSnapshot _wifiSnapshot;
  SdSpeedTestSnapshot _sdSnapshot;
  Mode _mode;
  bool _lastTouchDown;
  bool _closePending;
  bool _lastConnected;
  bool _lastSdMounted;
  bool _lastWorkerActive;
  int32_t _lastRssi;
  uint32_t _lastSignalDrawMs;
  Rect _wifiModeBounds;
  Rect _sdModeBounds;
  Rect _buttonBounds;

  void draw();
  void drawModeSelector();
  void drawModeContent();
  void refreshWiFi(const WiFiSpeedTestSnapshot &snapshot, bool connected,
                   int32_t rssi, bool workerActive);
  void refreshSd(const SdSpeedTestSnapshot &snapshot, bool mounted,
                 bool workerActive);
  void drawConnection();
  void drawMetrics();
  void drawMetric(uint8_t index);
  void drawProgress(bool full, uint8_t previousProgress = 0);
  void drawAction();
  void drawActionButton();
  bool workerActive() const;
  uint8_t progress() const;
  const char *status() const;
  bool contains(const Rect &bounds, int16_t x, int16_t y) const;
  String metricText(float value, uint8_t decimals, const char *suffix) const;
  const char *buttonLabel() const;
};
