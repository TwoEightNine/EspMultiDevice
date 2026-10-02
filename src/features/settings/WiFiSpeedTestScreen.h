#pragma once

#include "../../core/Screen.h"
#include "../../services/network/WiFiController.h"
#include "WiFiSpeedTestWorker.h"

class WiFiSpeedTestScreen : public Screen {
public:
  WiFiSpeedTestScreen(Navigator &navigator,
                      TouchController &touch,
                      DisplayController &display,
                      WiFiController &wifi);

  void onEnter() override;
  bool releaseResources() override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  struct Rect {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
  };

  WiFiController &_wifi;
  WiFiSpeedTestWorker _worker;
  WiFiSpeedTestSnapshot _snapshot;
  bool _lastTouchDown;
  bool _closePending;
  bool _lastConnected;
  int32_t _lastRssi;
  uint32_t _lastSignalDrawMs;
  Rect _buttonBounds;

  void draw();
  void drawConnection();
  void drawMetrics();
  void drawProgress();
  void drawAction();
  bool contains(const Rect &bounds, int16_t x, int16_t y) const;
  String metricText(float value, uint8_t decimals, const char *suffix) const;
  const char *buttonLabel() const;
};
