#include "Navigator.h"

#include "Screen.h"

Navigator::Navigator()
  : _registeredCount(0),
    _stackDepth(0) {
  for (uint8_t i = 0; i < MaxScreens; i++) {
    _registeredIds[i] = ScreenId::Home;
    _registeredScreens[i] = nullptr;
  }

  for (uint8_t i = 0; i < MaxStackDepth; i++) {
    _stack[i] = ScreenId::Home;
  }
}

bool Navigator::registerScreen(ScreenId id, Screen &screen) {
  if (_registeredCount >= MaxScreens || isRegistered(id)) {
    return false;
  }

  _registeredIds[_registeredCount] = id;
  _registeredScreens[_registeredCount] = &screen;
  _registeredCount++;
  return true;
}

bool Navigator::setRoot(ScreenId id) {
  if (!isRegistered(id)) {
    return false;
  }

  exitActive();
  _stack[0] = id;
  _stackDepth = 1;
  enter(id);
  return true;
}

bool Navigator::push(ScreenId id) {
  if (!isRegistered(id) || _stackDepth >= MaxStackDepth) {
    return false;
  }

  exitActive();
  _stack[_stackDepth] = id;
  _stackDepth++;
  enter(id);
  return true;
}

bool Navigator::replace(ScreenId id) {
  if (!isRegistered(id)) {
    return false;
  }

  if (_stackDepth == 0) {
    _stack[0] = id;
    _stackDepth = 1;
    enter(id);
    return true;
  }

  exitActive();
  _stack[_stackDepth - 1] = id;
  enter(id);
  return true;
}

bool Navigator::pop() {
  if (_stackDepth <= 1) {
    return false;
  }

  exitActive();
  _stackDepth--;
  enter(_stack[_stackDepth - 1]);
  return true;
}

Screen *Navigator::activeScreen() {
  if (!hasActiveScreen()) {
    return nullptr;
  }

  return screenFor(_stack[_stackDepth - 1]);
}

ScreenId Navigator::activeScreenId() const {
  if (!hasActiveScreen()) {
    return ScreenId::Home;
  }

  return _stack[_stackDepth - 1];
}

bool Navigator::hasActiveScreen() const {
  return _stackDepth > 0;
}

Screen *Navigator::screenFor(ScreenId id) const {
  for (uint8_t i = 0; i < _registeredCount; i++) {
    if (_registeredIds[i] == id) {
      return _registeredScreens[i];
    }
  }

  return nullptr;
}

bool Navigator::isRegistered(ScreenId id) const {
  return screenFor(id) != nullptr;
}

void Navigator::enter(ScreenId id) {
  Screen *screen = screenFor(id);
  if (screen) {
    screen->onEnter();
  }
}

void Navigator::exitActive() {
  Screen *screen = activeScreen();
  if (screen) {
    screen->onExit();
  }
}
