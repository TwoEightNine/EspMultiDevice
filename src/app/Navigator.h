#pragma once

#include <Arduino.h>

#include "../core/Navigation.h"

class Screen;

enum class ScreenId : uint8_t {
  Splash,
  Home,
  VoiceRecorder,
  Files,
  Settings,
  WiFi,
  WiFiSpeedTest,
  TouchCalibration,
  ImageViewer,
  AudioPlayer,
  VideoPlayer,
  TextViewer,
  TextInput,
  Browser,
  AudioSettings,
  Led,
  BatteryInfo,
  Workout,
  Assistant,
  UsbDrive,
  MacRemote,
  Apps,
  ExternalApp
};

class Navigator {
public:
  static const uint8_t MaxScreens = 23;
  static const uint8_t MaxStackDepth = 8;

  Navigator();

  bool registerScreen(ScreenId id, Screen &screen);
  bool setRoot(ScreenId id);
  bool push(ScreenId id);
  bool push(ScreenId id, const ScreenData &data);
  bool replace(ScreenId id);
  bool replace(ScreenId id, const ScreenData &data);
  bool pop();
  bool pop(const ScreenResult &result);
  void tick(uint32_t nowMs);

  Screen *activeScreen();
  ScreenId activeScreenId() const;
  bool hasActiveScreen() const;
  Screen *screenFor(ScreenId id) const;

private:
  ScreenId _registeredIds[MaxScreens];
  Screen *_registeredScreens[MaxScreens];
  uint8_t _registeredCount;
  struct StackEntry {
    ScreenId id;
    ScreenData data;
  };

  StackEntry _stack[MaxStackDepth];
  uint8_t _stackDepth;

  bool isRegistered(ScreenId id) const;
  void enter(const StackEntry &entry);
  void exitActive();
  bool exitAndReleaseActive();
  void resumeActive();
};
