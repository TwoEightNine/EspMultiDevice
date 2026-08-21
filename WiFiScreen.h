#pragma once

#include <Arduino.h>

#include "Screen.h"
#include "WiFiController.h"

class WiFiScreen : public Screen {
public:
  WiFiScreen(Navigator &navigator,
             TouchController &touch,
             DisplayController &display,
             WiFiController &wifi);

  void onEnter() override;
  void onResult(const ScreenResult &result) override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  struct Rect {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
  };

  static const uint8_t VisibleNetworkCount = 4;
  static const int16_t SwitchY = 68;
  static const int16_t SwitchW = 72;
  static const int16_t SwitchH = 34;

  WiFiController &_wifi;
  bool _lastTouchDown;
  bool _forceDraw;
  uint32_t _lastDrawMs;
  WiFiConnectionState _lastState;
  bool _lastEnabled;
  bool _lastScanning;
  uint8_t _lastNetworkCount;
  String _lastError;
  String _lastSsid;
  String _selectedSsid;
  Rect _switchBounds;
  Rect _forgetBounds;
  Rect _rescanBounds;

  void draw();
  void drawTopBar();
  void drawEnabledRow();
  void drawSwitch();
  void drawConnected();
  void drawDisconnected(uint32_t nowMs);
  void drawNetworkList();
  void drawButton(const Rect &bounds, const char *label, uint16_t fill);
  void drawLoader(const char *message, uint32_t nowMs);
  bool contains(const Rect &bounds, int16_t x, int16_t y) const;
  bool isLoaderVisible() const;
};
