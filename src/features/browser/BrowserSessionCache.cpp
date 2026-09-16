#include "BrowserSessionCache.h"

#include <FS.h>
#include <SD.h>
#include <freertos/task.h>

#include "../../core/SpiBusLock.h"
#include "../../services/storage/SdCardController.h"
#include "BrowserLimits.h"

using File = fs::File;

namespace {
const char *SessionCacheDirectory = "/.system/browser_session";

void cacheLog(const __FlashStringHelper *message) {
  Serial.print(F("[Browser] "));
  Serial.println(message);
}

void cacheLog(const String &message) {
  Serial.print(F("[Browser] "));
  Serial.println(message);
}

class CacheLock {
public:
  explicit CacheLock(SemaphoreHandle_t mutex)
      : _mutex(mutex), _locked(mutex && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {}
  ~CacheLock() { if (_locked) xSemaphoreGive(_mutex); }
  bool locked() const { return _locked; }

private:
  SemaphoreHandle_t _mutex;
  bool _locked;
};
}  // namespace

BrowserSessionCache::BrowserSessionCache(SdCardController &sdCard)
    : _sdCard(sdCard),
      _sessionCacheActive(false),
      _sessionCacheInitialized(false),
      _sessionCacheBytes(0),
      _sessionCacheClock(0),
      _sessionCacheSequence(0),
      _sessionCacheMutex(xSemaphoreCreateMutex()) {
  resetCacheState();
}

void BrowserSessionCache::beginSession() {
  _sessionCacheActive = true;
  _sessionCacheInitialized = false;
}

void BrowserSessionCache::endSession() {
  CacheLock lock(_sessionCacheMutex);
  _sessionCacheActive = false;
  _sessionCacheInitialized = false;
  if (lock.locked()) resetCacheState();
}

bool BrowserSessionCache::prepareSession() {
  if (!_sessionCacheActive || !_sdCard.isMounted()) return false;
  CacheLock lock(_sessionCacheMutex);
  if (!lock.locked() || !_sessionCacheActive || !_sdCard.isMounted()) return false;
  if (_sessionCacheInitialized) return true;
  _sessionCacheInitialized = clearSessionCache();
  return _sessionCacheInitialized;
}

void BrowserSessionCache::resetCacheState() {
  _sessionCacheBytes = 0;
  _sessionCacheClock = 0;
  _sessionCacheSequence = 0;
  for (auto &page : _sessionPages) {
    page.url = static_cast<const char *>(nullptr);
    page.contentType = static_cast<const char *>(nullptr);
    page.path = static_cast<const char *>(nullptr);
    page.bytes = page.lastUse = 0;
    page.used = false;
  }
  for (auto &image : _sessionImages) {
    image.url = static_cast<const char *>(nullptr);
    image.path = static_cast<const char *>(nullptr);
    image.bytes = image.lastUse = 0;
    image.used = false;
  }
}

bool BrowserSessionCache::clearSessionCache() {
  resetCacheState();
  if (!_sdCard.isMounted() || !SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
  File directory = SD.open(SessionCacheDirectory, FILE_READ);
  const bool isDirectory = directory && directory.isDirectory();
  SpiBusLock::unlock();
  if (isDirectory) {
    while (true) {
      if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
      File item = directory.openNextFile();
      String path = item ? String(item.name()) : String();
      if (item) item.close();
      SpiBusLock::unlock();
      if (path.length() == 0) break;
      // Filesystems disagree on whether this is a full path or filename.
      if (!path.startsWith("/")) {
        path = path.startsWith(".system/") ? "/" + path : String(SessionCacheDirectory) + "/" + path;
      }
      if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
      SD.remove(path.c_str());
      SpiBusLock::unlock();
      // Leave time for display and touch between FAT changes.
      vTaskDelay(1);
    }
    if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
    directory.close();
    SD.rmdir(SessionCacheDirectory);
    SpiBusLock::unlock();
  } else if (directory) {
    if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
    directory.close();
    SD.remove(SessionCacheDirectory);
    SpiBusLock::unlock();
  }
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
  const bool ready = (SD.exists("/.system") || SD.mkdir("/.system")) &&
                     (SD.exists(SessionCacheDirectory) || SD.mkdir(SessionCacheDirectory));
  SpiBusLock::unlock();
  return ready;
}

bool BrowserSessionCache::removeSessionFile(const String &path) {
  if (path.length() == 0) return true;
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
  const bool removed = !SD.exists(path.c_str()) || SD.remove(path.c_str());
  SpiBusLock::unlock();
  return removed;
}

void BrowserSessionCache::removeSessionPage(uint8_t index) {
  if (index >= BrowserLimits::MaxSessionPages || !_sessionPages[index].used) return;
  if (_sessionCacheBytes >= _sessionPages[index].bytes) _sessionCacheBytes -= _sessionPages[index].bytes;
  else _sessionCacheBytes = 0;
  _sessionPages[index] = {"", "", "", 0, 0, false};
}

void BrowserSessionCache::removeSessionImage(uint8_t index) {
  if (index >= BrowserLimits::MaxSessionImages || !_sessionImages[index].used) return;
  if (_sessionCacheBytes >= _sessionImages[index].bytes) _sessionCacheBytes -= _sessionImages[index].bytes;
  else _sessionCacheBytes = 0;
  _sessionImages[index] = {"", "", 0, 0, false};
}

bool BrowserSessionCache::evictOldestSessionEntry() {
  bool page = false;
  int16_t index = -1;
  uint32_t oldest = 0xffffffffUL;
  for (uint8_t i = 0; i < BrowserLimits::MaxSessionPages; ++i) {
    if (_sessionPages[i].used && _sessionPages[i].lastUse <= oldest) {
      page = true;
      index = i;
      oldest = _sessionPages[i].lastUse;
    }
  }
  for (uint8_t i = 0; i < BrowserLimits::MaxSessionImages; ++i) {
    if (_sessionImages[i].used && _sessionImages[i].lastUse <= oldest) {
      page = false;
      index = i;
      oldest = _sessionImages[i].lastUse;
    }
  }
  if (index < 0) return false;
  const String path = page ? _sessionPages[index].path : _sessionImages[index].path;
  if (!removeSessionFile(path)) return false;
  if (page) removeSessionPage((uint8_t)index);
  else removeSessionImage((uint8_t)index);
  cacheLog(String(F("session cache evicted: ")) + path);
  return true;
}

bool BrowserSessionCache::evictOldestSessionImage() {
  int16_t index = -1;
  uint32_t oldest = 0xffffffffUL;
  for (uint8_t i = 0; i < BrowserLimits::MaxSessionImages; ++i) {
    if (_sessionImages[i].used && _sessionImages[i].lastUse <= oldest) {
      index = i;
      oldest = _sessionImages[i].lastUse;
    }
  }
  if (index < 0 || !removeSessionFile(_sessionImages[index].path)) return false;
  removeSessionImage((uint8_t)index);
  cacheLog(F("session image cache evicted to retain browser pages"));
  return true;
}

bool BrowserSessionCache::ensureSessionCacheSpace(uint32_t bytes, bool preservePages) {
  if (bytes > BrowserLimits::MaxSessionCacheBytes) return false;
  while (_sessionCacheBytes > BrowserLimits::MaxSessionCacheBytes - bytes) {
    // Page bodies are the Back cache; previews must never displace them.
    if (preservePages ? !evictOldestSessionImage() : !evictOldestSessionEntry()) return false;
  }
  return true;
}

uint32_t BrowserSessionCache::nextSessionCacheUse() {
  ++_sessionCacheClock;
  if (_sessionCacheClock == 0) _sessionCacheClock = 1;
  return _sessionCacheClock;
}

String BrowserSessionCache::nextSessionCachePath(const char *prefix, const char *extension) {
  ++_sessionCacheSequence;
  if (_sessionCacheSequence == 0) _sessionCacheSequence = 1;
  return String(SessionCacheDirectory) + "/" + prefix + String((unsigned long)_sessionCacheSequence) + extension;
}

int8_t BrowserSessionCache::sessionPageIndex(const String &url) const {
  for (uint8_t i = 0; i < BrowserLimits::MaxSessionPages; ++i) {
    if (_sessionPages[i].used && _sessionPages[i].url == url) return (int8_t)i;
  }
  return -1;
}

int8_t BrowserSessionCache::sessionImageIndex(const String &url) const {
  for (uint8_t i = 0; i < BrowserLimits::MaxSessionImages; ++i) {
    if (_sessionImages[i].used && _sessionImages[i].url == url) return (int8_t)i;
  }
  return -1;
}

bool BrowserSessionCache::loadPage(const String &url, String &body, String &contentType,
                                   const Browser::CancellationCheck *cancellation) {
  body = "";
  contentType = "";
  if (!_sessionCacheActive || !_sessionCacheInitialized || !_sdCard.isMounted()) return false;
  CacheLock lock(_sessionCacheMutex);
  if (!lock.locked() || !_sessionCacheActive || !_sessionCacheInitialized || !_sdCard.isMounted()) return false;
  const int8_t index = sessionPageIndex(url);
  if (index < 0) return false;
  const uint32_t cacheStartedMs = millis();
  Browser::SessionPageCacheEntry &entry = _sessionPages[index];
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
  File file = SD.open(entry.path.c_str(), FILE_READ);
  const uint32_t size = file ? (uint32_t)file.size() : 0xffffffffUL;
  SpiBusLock::unlock();
  if (!file || size != entry.bytes || size > BrowserLimits::MaxBodyBytes || !body.reserve(size)) {
    cacheLog(String(F("session page cache entry invalid: url=")) + url + F(", path=") + entry.path +
             F(", expectedBytes=") + String(entry.bytes) + F(", actualBytes=") + String(size));
    if (file && SpiBusLock::lock()) {
      file.close();
      SpiBusLock::unlock();
    }
    if (removeSessionFile(entry.path)) removeSessionPage((uint8_t)index);
    return false;
  }
  uint8_t buffer[BrowserLimits::CacheIoChunkBytes];
  uint32_t offset = 0;
  bool readOk = true;
  while (offset < size) {
    if (cancellation && cancellation->cancelled()) { readOk = false; break; }
    const uint16_t amount = size - offset > sizeof(buffer) ? sizeof(buffer) : (uint16_t)(size - offset);
    if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) { readOk = false; break; }
    const int received = file.read(buffer, amount);
    SpiBusLock::unlock();
    if (received != amount || !body.concat(reinterpret_cast<const char *>(buffer), amount)) {
      readOk = false;
      break;
    }
    offset += amount;
  }
  if (SpiBusLock::lock(pdMS_TO_TICKS(100))) {
    file.close();
    SpiBusLock::unlock();
  }
  if (!readOk) {
    if (cancellation && cancellation->cancelled()) {
      body = "";
      return false;
    }
    cacheLog(String(F("session page cache read failed: ")) + entry.path);
    if (removeSessionFile(entry.path)) removeSessionPage((uint8_t)index);
    body = "";
    return false;
  }
  entry.lastUse = nextSessionCacheUse();
  contentType = entry.contentType;
  cacheLog(String(F("session page cache loaded: bytes=")) + String(size) +
           F(", elapsedMs=") + String(millis() - cacheStartedMs));
  return true;
}

bool BrowserSessionCache::storePage(const String &url, const String &contentType, const String &body,
                                    const Browser::CancellationCheck *cancellation) {
  if (body.length() > BrowserLimits::MaxBodyBytes || !_sessionCacheActive ||
      !_sessionCacheInitialized || !_sdCard.isMounted()) return false;
  CacheLock lock(_sessionCacheMutex);
  if (!lock.locked() || !_sessionCacheActive || !_sessionCacheInitialized || !_sdCard.isMounted()) return false;
  const int8_t existing = sessionPageIndex(url);
  if (existing >= 0) {
    if (!removeSessionFile(_sessionPages[existing].path)) return false;
    removeSessionPage((uint8_t)existing);
  }
  if (!ensureSessionCacheSpace(body.length(), true)) return false;
  int8_t slot = -1;
  for (uint8_t i = 0; i < BrowserLimits::MaxSessionPages; ++i) {
    if (!_sessionPages[i].used) { slot = i; break; }
  }
  if (slot < 0) {
    uint8_t oldest = 0;
    for (uint8_t i = 1; i < BrowserLimits::MaxSessionPages; ++i) {
      if (_sessionPages[i].lastUse < _sessionPages[oldest].lastUse) oldest = i;
    }
    if (!removeSessionFile(_sessionPages[oldest].path)) return false;
    removeSessionPage(oldest);
    slot = oldest;
  }
  const String path = nextSessionCachePath("page-", ".html");
  const String temporary = path + F(".tmp");
  const uint32_t cacheStartedMs = millis();
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
  SD.remove(temporary.c_str());
  File file = SD.open(temporary.c_str(), FILE_WRITE);
  SpiBusLock::unlock();
  if (!file) return false;
  uint32_t offset = 0;
  bool written = true;
  while (offset < body.length()) {
    if (cancellation && cancellation->cancelled()) { written = false; break; }
    const uint16_t amount = body.length() - offset > BrowserLimits::CacheIoChunkBytes
      ? BrowserLimits::CacheIoChunkBytes : (uint16_t)(body.length() - offset);
    if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) { written = false; break; }
    const size_t count = file.write(reinterpret_cast<const uint8_t *>(body.c_str()) + offset, amount);
    SpiBusLock::unlock();
    if (count != amount) { written = false; break; }
    offset += amount;
    yield();
  }
  if (SpiBusLock::lock(pdMS_TO_TICKS(100))) {
    file.close();
    SpiBusLock::unlock();
  } else {
    written = false;
  }
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
  bool committed = false;
  if (written) {
    const bool targetReady = !SD.exists(path.c_str()) || SD.remove(path.c_str());
    committed = targetReady && SD.rename(temporary.c_str(), path.c_str());
  }
  if (!committed) SD.remove(temporary.c_str());
  SpiBusLock::unlock();
  if (!committed) return false;
  _sessionPages[slot] = {url, contentType, path, body.length(), nextSessionCacheUse(), true};
  _sessionCacheBytes += body.length();
  cacheLog(String(F("page stored in session cache: ")) + url + F(", bytes=") +
           String(body.length()) + F(", elapsedMs=") + String(millis() - cacheStartedMs));
  return true;
}

String BrowserSessionCache::findImage(const String &url) {
  if (!_sessionCacheActive || !_sessionCacheInitialized || !_sdCard.isMounted()) return "";
  CacheLock lock(_sessionCacheMutex);
  if (!lock.locked() || !_sessionCacheActive || !_sessionCacheInitialized || !_sdCard.isMounted()) return "";
  const int8_t index = sessionImageIndex(url);
  if (index < 0) return "";
  Browser::SessionImageCacheEntry &entry = _sessionImages[index];
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return "";
  const bool exists = SD.exists(entry.path.c_str());
  SpiBusLock::unlock();
  if (!exists) {
    removeSessionImage((uint8_t)index);
    return "";
  }
  entry.lastUse = nextSessionCacheUse();
  return entry.path;
}

void BrowserSessionCache::discardImage(const String &url) {
  if (!_sessionCacheActive || !_sessionCacheInitialized || !_sdCard.isMounted()) return;
  CacheLock lock(_sessionCacheMutex);
  if (!lock.locked() || !_sessionCacheActive || !_sessionCacheInitialized || !_sdCard.isMounted()) return;
  const int8_t index = sessionImageIndex(url);
  if (index >= 0 && removeSessionFile(_sessionImages[index].path)) removeSessionImage((uint8_t)index);
}

String BrowserSessionCache::adoptImage(const String &url, const String &downloadedPath,
                                       FileFormats::Format format) {
  if (!FileFormats::isImage(format) || !_sessionCacheActive || !_sessionCacheInitialized ||
      !_sdCard.isMounted()) return "";
  CacheLock lock(_sessionCacheMutex);
  if (!lock.locked() || !_sessionCacheActive || !_sessionCacheInitialized || !_sdCard.isMounted()) return "";
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return "";
  File source = SD.open(downloadedPath.c_str(), FILE_READ);
  const uint32_t bytes = source ? (uint32_t)source.size() : 0;
  if (source) source.close();
  SpiBusLock::unlock();
  if (bytes == 0) return "";
  const int8_t existing = sessionImageIndex(url);
  if (existing >= 0) {
    if (!removeSessionFile(_sessionImages[existing].path)) return "";
    removeSessionImage((uint8_t)existing);
  }
  if (!ensureSessionCacheSpace(bytes, true)) return "";
  int8_t slot = -1;
  for (uint8_t i = 0; i < BrowserLimits::MaxSessionImages; ++i) {
    if (!_sessionImages[i].used) { slot = i; break; }
  }
  if (slot < 0) {
    uint8_t oldest = 0;
    for (uint8_t i = 1; i < BrowserLimits::MaxSessionImages; ++i) {
      if (_sessionImages[i].lastUse < _sessionImages[oldest].lastUse) oldest = i;
    }
    if (!removeSessionFile(_sessionImages[oldest].path)) return "";
    removeSessionImage(oldest);
    slot = oldest;
  }
  const String path = nextSessionCachePath("image-", FileFormats::extension(format));
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return "";
  const bool moved = !SD.exists(path.c_str()) && SD.rename(downloadedPath.c_str(), path.c_str());
  SpiBusLock::unlock();
  if (!moved) return "";
  _sessionImages[slot] = {url, path, bytes, nextSessionCacheUse(), true};
  _sessionCacheBytes += bytes;
  cacheLog(String(F("image stored in session cache: ")) + url);
  return path;
}
