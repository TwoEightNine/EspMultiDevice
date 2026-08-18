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

  SdCardController &_sdCard;
  bool _sdMounted;
  String _sdDetail;
  String _touchLog[TouchLogLines];
  uint8_t _touchLogCount;
  uint8_t _touchLogNext;
  uint32_t _touchEventCount;

  void draw();
  void redrawTouchLog();
  void logTouch(const String &message);
  void setSdState(bool mounted, const String &detail);
  void refreshSdState();
  void runSdSelfTest();
  void handleSdCardEvent(SdCardEvent event);
};
