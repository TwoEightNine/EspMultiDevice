#pragma once

#include "../../core/Screen.h"
#include "AppPackage.h"

#include <SD.h>

class SdCardController;

class AppLauncherScreen : public Screen {
public:
  AppLauncherScreen(Navigator &navigator, TouchController &touch,
                    DisplayController &display, SdCardController &card);
  void onEnter() override;
  void onExit() override;
  void onResume() override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  static constexpr uint8_t MaxApps = 32;
  static constexpr uint8_t PageSize = 8;
  static constexpr uint8_t MaxScanned = 128;
  SdCardController &_card;
  File _directory;
  AppPackage _apps[MaxApps];
  uint8_t _count = 0;
  uint8_t _page = 0;
  uint8_t _scanned = 0;
  bool _scanning = false;
  bool _mounted = false;
  bool _lastTouchDown = false;
  String _message;

  void startScan();
  void stopScan();
  void scanOne();
  void draw();
};
