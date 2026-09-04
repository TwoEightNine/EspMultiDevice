#include "Navigator.h"

#include "../core/Screen.h"

Navigator::Navigator()
  : _registeredCount(0),
    _stackDepth(0) {
  for (uint8_t i = 0; i < MaxScreens; i++) {
    _registeredIds[i] = ScreenId::Home;
    _registeredScreens[i] = nullptr;
  }

  for (uint8_t i = 0; i < MaxStackDepth; i++) {
    _stack[i].id = ScreenId::Home;
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
  _stack[0].id = id;
  _stack[0].data = ScreenData();
  _stackDepth = 1;
  enter(_stack[0]);
  return true;
}

bool Navigator::push(ScreenId id) {
  return push(id, ScreenData());
}

bool Navigator::push(ScreenId id, const ScreenData &data) {
  if (!isRegistered(id) || _stackDepth >= MaxStackDepth) {
    return false;
  }

  exitActive();
  _stack[_stackDepth].id = id;
  _stack[_stackDepth].data = data;
  _stackDepth++;
  enter(_stack[_stackDepth - 1]);
  return true;
}

bool Navigator::replace(ScreenId id) {
  return replace(id, ScreenData());
}

bool Navigator::replace(ScreenId id, const ScreenData &data) {
  if (!isRegistered(id)) {
    return false;
  }

  if (_stackDepth == 0) {
    _stack[0].id = id;
    _stack[0].data = data;
    _stackDepth = 1;
    enter(_stack[0]);
    return true;
  }

  exitActive();
  _stack[_stackDepth - 1].id = id;
  _stack[_stackDepth - 1].data = data;
  enter(_stack[_stackDepth - 1]);
  return true;
}

bool Navigator::pop() {
  if (_stackDepth <= 1) {
    return false;
  }

  exitActive();
  _stackDepth--;
  resumeActive();
  return true;
}

bool Navigator::pop(const ScreenResult &result) {
  if (_stackDepth <= 1) {
    return false;
  }

  exitActive();
  _stackDepth--;
  resumeActive();
  Screen *screen = activeScreen();
  if (screen) {
    screen->onResult(result);
  }
  return true;
}

Screen *Navigator::activeScreen() {
  if (!hasActiveScreen()) {
    return nullptr;
  }

  return screenFor(_stack[_stackDepth - 1].id);
}

ScreenId Navigator::activeScreenId() const {
  if (!hasActiveScreen()) {
    return ScreenId::Home;
  }

  return _stack[_stackDepth - 1].id;
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

void Navigator::enter(const StackEntry &entry) {
  Screen *screen = screenFor(entry.id);
  if (screen) {
    screen->onEnter(entry.data);
  }
}

void Navigator::exitActive() {
  Screen *screen = activeScreen();
  if (screen) {
    screen->onExit();
  }
}

void Navigator::resumeActive() {
  Screen *screen = activeScreen();
  if (screen) {
    screen->onResume();
  }
}
