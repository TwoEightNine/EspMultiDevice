#pragma once

#include <Arduino.h>
#include <freertos/semphr.h>

#include "BrowserTypes.h"

// Owns the bounded, disposable browser-session cache state.  Cache I/O stays
// in the browser feature for this mechanical first move, but no screen now
// owns the cache's records, accounting, or synchronization primitive.
class BrowserSessionCache {
public:
  BrowserSessionCache();

  void startSession();
  void endSession();
  bool active() const { return _sessionCacheActive; }
  bool initialized() const { return _sessionCacheInitialized; }

protected:
  void resetCacheState();

  volatile bool _sessionCacheActive;
  volatile bool _sessionCacheInitialized;
  uint32_t _sessionCacheBytes;
  uint32_t _sessionCacheClock;
  uint32_t _sessionCacheSequence;
  Browser::SessionPageCacheEntry _sessionPages[BrowserLimits::MaxSessionPages];
  Browser::SessionImageCacheEntry _sessionImages[BrowserLimits::MaxSessionImages];
  SemaphoreHandle_t _sessionCacheMutex;
};
