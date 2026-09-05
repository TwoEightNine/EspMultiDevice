#pragma once

#include <Arduino.h>
#include <freertos/semphr.h>

#include "../../media/FileFormats.h"
#include "BrowserTypes.h"

class SdCardController;

// Owns the bounded, disposable browser-session cache, including its files,
// LRU accounting, and synchronization. BrowserScreen only coordinates the
// cache with navigation and its workers.
class BrowserSessionCache {
public:
  explicit BrowserSessionCache(SdCardController &sdCard);

  void beginSession();
  void endSession();
  bool prepareSession();
  bool active() const { return _sessionCacheActive; }

  bool loadPage(const String &url, String &body, String &contentType,
                const Browser::CancellationCheck *cancellation = nullptr);
  bool storePage(const String &url, const String &contentType, const String &body,
                 const Browser::CancellationCheck *cancellation = nullptr);
  String findImage(const String &url);
  String adoptImage(const String &url, const String &downloadedPath, FileFormats::Format format);
  void discardImage(const String &url);

private:
  SdCardController &_sdCard;
  void resetCacheState();
  bool clearSessionCache();
  bool removeSessionFile(const String &path);
  bool ensureSessionCacheSpace(uint32_t bytes, bool preservePages = false);
  bool evictOldestSessionEntry();
  bool evictOldestSessionImage();
  void removeSessionPage(uint8_t index);
  void removeSessionImage(uint8_t index);
  uint32_t nextSessionCacheUse();
  String nextSessionCachePath(const char *prefix, const char *extension);
  int8_t sessionPageIndex(const String &url) const;
  int8_t sessionImageIndex(const String &url) const;

  volatile bool _sessionCacheActive;
  volatile bool _sessionCacheInitialized;
  uint32_t _sessionCacheBytes;
  uint32_t _sessionCacheClock;
  uint32_t _sessionCacheSequence;
  Browser::SessionPageCacheEntry _sessionPages[BrowserLimits::MaxSessionPages];
  Browser::SessionImageCacheEntry _sessionImages[BrowserLimits::MaxSessionImages];
  SemaphoreHandle_t _sessionCacheMutex;
};
