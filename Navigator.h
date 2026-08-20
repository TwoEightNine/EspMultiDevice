#pragma once

#include <Arduino.h>

class Screen;

enum class ScreenId : uint8_t {
  Splash,
  Home,
  VoiceRecorder,
  Files
};

class Navigator {
public:
  static const uint8_t MaxScreens = 8;
  static const uint8_t MaxStackDepth = 8;

  Navigator();

  bool registerScreen(ScreenId id, Screen &screen);
  bool setRoot(ScreenId id);
  bool push(ScreenId id);
  bool replace(ScreenId id);
  bool pop();

  Screen *activeScreen();
  ScreenId activeScreenId() const;
  bool hasActiveScreen() const;

private:
  ScreenId _registeredIds[MaxScreens];
  Screen *_registeredScreens[MaxScreens];
  uint8_t _registeredCount;
  ScreenId _stack[MaxStackDepth];
  uint8_t _stackDepth;

  Screen *screenFor(ScreenId id) const;
  bool isRegistered(ScreenId id) const;
  void enter(ScreenId id);
  void exitActive();
};
