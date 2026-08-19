#pragma once

#include <Arduino.h>

#include "SdCardController.h"
#include "Screen.h"

class HomeScreen : public Screen {
public:
  HomeScreen(Navigator &navigator,
             TouchController &touch,
             DisplayController &display,
             SdCardController &sdCard);

  void onEnter() override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  enum {
    TouchLogLines = 5
  };

  struct Button {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;

    bool contains(int16_t px, int16_t py) const {
      return px >= x && px < x + w && py >= y && py < y + h;
    }
  };

  SdCardController &_sdCard;
  bool _sdMounted;
  String _sdDetail;
  String _touchLog[TouchLogLines];
  uint8_t _touchLogCount;
  uint8_t _touchLogNext;
  uint32_t _touchEventCount;
  bool _lastTouchDown;
  Button _recorderButton;

  void draw();
  void layoutButtons();
  void drawRecorderButton();
  void redrawTouchLog();
  void logTouch(const String &message);
  void setSdState(bool mounted, const String &detail);
  void refreshSdState();
  void runSdSelfTest();
  void handleSdCardEvent(SdCardEvent event);
};
