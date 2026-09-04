#include "BrowserSessionCache.h"

BrowserSessionCache::BrowserSessionCache()
    : _sessionCacheActive(false),
      _sessionCacheInitialized(false),
      _sessionCacheBytes(0),
      _sessionCacheClock(0),
      _sessionCacheSequence(0),
      _sessionCacheMutex(xSemaphoreCreateMutex()) {
  resetCacheState();
}

void BrowserSessionCache::startSession() {
  _sessionCacheActive = true;
  _sessionCacheInitialized = false;
}

void BrowserSessionCache::endSession() {
  _sessionCacheActive = false;
  _sessionCacheInitialized = false;
}

void BrowserSessionCache::resetCacheState() {
  _sessionCacheBytes = 0;
  _sessionCacheClock = 0;
  _sessionCacheSequence = 0;
  for (uint8_t i = 0; i < BrowserLimits::MaxSessionPages; ++i)
    _sessionPages[i] = {"", "", "", 0, 0, false};
  for (uint8_t i = 0; i < BrowserLimits::MaxSessionImages; ++i)
    _sessionImages[i] = {"", "", 0, 0, false};
}
