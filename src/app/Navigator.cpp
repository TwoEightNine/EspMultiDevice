#include "Navigator.h"

#include "../core/Screen.h"

namespace {
struct HeapSnapshot {
  uint32_t freeHeap;
  uint32_t largestHeap;
  uint32_t freePsram;
  uint32_t largestPsram;
};

HeapSnapshot heapSnapshot() {
  return {ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getFreePsram(), ESP.getMaxAllocPsram()};
}
}  // namespace

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

  while (_stackDepth > 0) {
    if (!exitAndReleaseActive()) return false;
    _stackDepth--;
  }
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

  if (!exitAndReleaseActive()) return false;
  _stack[_stackDepth - 1].id = id;
  _stack[_stackDepth - 1].data = data;
  enter(_stack[_stackDepth - 1]);
  return true;
}

bool Navigator::pop() {
  if (_stackDepth <= 1) {
    return false;
  }

  if (!exitAndReleaseActive()) return false;
  _stackDepth--;
  resumeActive();
  return true;
}

bool Navigator::pop(const ScreenResult &result) {
  if (_stackDepth <= 1) {
    return false;
  }

  if (!exitAndReleaseActive()) return false;
  _stackDepth--;
  resumeActive();
  Screen *screen = activeScreen();
  if (screen) {
    screen->onResult(result);
  }
  return true;
}

void Navigator::tick(uint32_t nowMs) {
  if (_stackDepth == 0) return;
  for (uint8_t index = 0; index + 1 < _stackDepth; ++index) {
    Screen *screen = screenFor(_stack[index].id);
    if (screen) screen->tickSuspended(nowMs);
  }
  Screen *screen = activeScreen();
  if (screen) screen->tick(nowMs);
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

bool Navigator::exitAndReleaseActive() {
  Screen *screen = activeScreen();
  if (!screen) return true;
  const HeapSnapshot before = heapSnapshot();
  screen->onExit();
  if (!screen->releaseResources()) return false;
  const HeapSnapshot after = heapSnapshot();
  Serial.printf("[Navigator] release screen=%u heap=%u->%u largest=%u->%u psram=%u->%u "
                "largest_psram=%u->%u\n",
                static_cast<unsigned>(activeScreenId()), before.freeHeap, after.freeHeap,
                before.largestHeap, after.largestHeap, before.freePsram, after.freePsram,
                before.largestPsram, after.largestPsram);
  return true;
}

void Navigator::resumeActive() {
  Screen *screen = activeScreen();
  if (screen) {
    screen->onResume();
  }
}
