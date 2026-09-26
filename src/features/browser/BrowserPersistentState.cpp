#include "BrowserPersistentState.h"

#include <FS.h>
#include <SD.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../../core/SpiBusLock.h"
#include "../../services/storage/SdCardController.h"
#include "BrowserLog.h"

namespace {
using File = fs::File;
constexpr const char *StatePath = "/.system/browser_state.txt";
constexpr const char *StateTempPath = "/.system/browser_state.tmp";

void stateLog(const String &message) {
  Serial.print(F("[Browser state] "));
  Serial.println(browserRedactUrls(message));
}
}  // namespace

BrowserPersistentState::BrowserPersistentState(SdCardController &sdCard)
    : _sdCard(sdCard), _recentCount(0), _favoriteCount(0), _cookieCount(0) {}

void BrowserPersistentState::releaseMemory() {
  for (auto &site : _recentSites) {
    site.url = static_cast<const char *>(nullptr);
    site.title = static_cast<const char *>(nullptr);
  }
  for (auto &site : _favoriteSites) {
    site.url = static_cast<const char *>(nullptr);
    site.title = static_cast<const char *>(nullptr);
  }
  for (auto &cookie : _cookies) {
    cookie.name = static_cast<const char *>(nullptr);
    cookie.value = static_cast<const char *>(nullptr);
    cookie.domain = static_cast<const char *>(nullptr);
    cookie.path = static_cast<const char *>(nullptr);
    cookie.secure = false;
    cookie.expiresAt = 0;
  }
  _recentCount = 0;
  _favoriteCount = 0;
  _cookieCount = 0;
}

const Browser::RecentSite &BrowserPersistentState::recentAt(uint8_t index) const {
  static const Browser::RecentSite Empty{"", ""};
  return index < _recentCount ? _recentSites[index] : Empty;
}

const Browser::RecentSite &BrowserPersistentState::favoriteAt(uint8_t index) const {
  static const Browser::RecentSite Empty{"", ""};
  return index < _favoriteCount ? _favoriteSites[index] : Empty;
}

void BrowserPersistentState::addRecent(const String &url, const String &title) {
  uint8_t found = _recentCount;
  for (uint8_t i = 0; i < _recentCount; ++i) if (_recentSites[i].url == url) { found = i; break; }
  if (found < _recentCount) {
    for (uint8_t i = found; i > 0; --i) _recentSites[i] = _recentSites[i - 1];
  } else {
    if (_recentCount < BrowserLimits::MaxRecentSites) ++_recentCount;
    for (uint8_t i = _recentCount - 1; i > 0; --i) _recentSites[i] = _recentSites[i - 1];
  }
  _recentSites[0] = {url, title};
}

void BrowserPersistentState::removeRecent(uint8_t index) {
  if (index >= _recentCount) return;
  for (uint8_t next = index + 1; next < _recentCount; ++next) _recentSites[next - 1] = _recentSites[next];
  --_recentCount;
  _recentSites[_recentCount] = {"", ""};
}

bool BrowserPersistentState::isFavorite(const String &url) const {
  for (uint8_t i = 0; i < _favoriteCount; ++i) if (_favoriteSites[i].url == url) return true;
  return false;
}

void BrowserPersistentState::addFavorite(const String &url, const String &title) {
  if (_favoriteCount >= BrowserLimits::MaxRecentSites || isFavorite(url)) return;
  _favoriteSites[_favoriteCount++] = {url, title};
}

void BrowserPersistentState::removeFavorite(const String &url) {
  for (uint8_t i = 0; i < _favoriteCount; ++i) {
    if (_favoriteSites[i].url != url) continue;
    for (uint8_t next = i + 1; next < _favoriteCount; ++next) _favoriteSites[next - 1] = _favoriteSites[next];
    --_favoriteCount;
    _favoriteSites[_favoriteCount] = {"", ""};
    return;
  }
}

String BrowserPersistentState::cookieHeaderFor(const Browser::Url &url, uint64_t nowEpoch) const {
  String result;
  for (uint8_t i = 0; i < _cookieCount; ++i) {
    const Browser::Cookie &cookie = _cookies[i];
    if (cookie.expiresAt == 1 || (nowEpoch && cookie.expiresAt && cookie.expiresAt <= nowEpoch)) continue;
    const bool domainMatch = url.host == cookie.domain ||
      (cookie.domain.length() && url.host.endsWith("." + cookie.domain));
    const String cookiePath = cookie.path.length() ? cookie.path : "/";
    const bool pathMatch = url.path.startsWith(cookiePath) &&
      (cookiePath.endsWith("/") || url.path.length() == cookiePath.length() || url.path[cookiePath.length()] == '/');
    if (!domainMatch || !pathMatch || (cookie.secure && url.scheme != "https")) continue;
    if (result.length()) result += "; ";
    result += cookie.name + "=" + cookie.value;
  }
  return result;
}

void BrowserPersistentState::acceptCookie(const String &header, const Browser::Url &url, uint64_t nowEpoch) {
  int separator = header.indexOf(';');
  String pair = trimCopy(header.substring(0, separator < 0 ? header.length() : separator));
  int equals = pair.indexOf('=');
  if (equals <= 0) return;
  Browser::Cookie incoming{};
  incoming.name = trimCopy(pair.substring(0, equals));
  incoming.value = pair.substring(equals + 1);
  incoming.domain = url.host;
  incoming.path = "/";
  incoming.secure = false;
  incoming.expiresAt = 0;
  int start = separator < 0 ? header.length() : separator + 1;
  while (start < (int)header.length()) {
    int end = header.indexOf(';', start);
    String attribute = trimCopy(header.substring(start, end < 0 ? header.length() : end));
    int attributeEquals = attribute.indexOf('=');
    String name = lower(trimCopy(attribute.substring(0, attributeEquals < 0 ? attribute.length() : attributeEquals)));
    String value = attributeEquals < 0 ? "" : trimCopy(attribute.substring(attributeEquals + 1));
    if (name == "domain" && value.length()) {
      value = lower(value);
      if (value.startsWith(".")) value.remove(0, 1);
      incoming.domain = value;
    } else if (name == "path" && value.startsWith("/")) incoming.path = value;
    else if (name == "secure") incoming.secure = true;
    else if (name == "max-age") {
      const long seconds = value.toInt();
      incoming.expiresAt = seconds <= 0 ? 1 : (nowEpoch ? nowEpoch + (uint64_t)seconds : 0);
    } else if (name == "expires" && !incoming.expiresAt) {
      const uint64_t expiry = parseHttpDate(value);
      if (expiry) incoming.expiresAt = expiry;
    }
    if (end < 0) break;
    start = end + 1;
  }
  if (!incoming.name.length() ||
      !(url.host == incoming.domain || url.host.endsWith("." + incoming.domain))) return;
  for (uint8_t i = 0; i < _cookieCount; ++i) {
    if (_cookies[i].name == incoming.name && _cookies[i].domain == incoming.domain && _cookies[i].path == incoming.path) {
      _cookies[i] = incoming;
      pruneCookies(nowEpoch);
      return;
    }
  }
  if (_cookieCount < BrowserLimits::MaxCookies) _cookies[_cookieCount++] = incoming;
  else {
    for (uint8_t i = 1; i < BrowserLimits::MaxCookies; ++i) _cookies[i - 1] = _cookies[i];
    _cookies[BrowserLimits::MaxCookies - 1] = incoming;
  }
  pruneCookies(nowEpoch);
}

void BrowserPersistentState::pruneCookies(uint64_t nowEpoch) {
  for (uint8_t i = 0; i < _cookieCount;) {
    if (_cookies[i].expiresAt == 1 || (nowEpoch && _cookies[i].expiresAt && _cookies[i].expiresAt <= nowEpoch)) {
      for (uint8_t j = i + 1; j < _cookieCount; ++j) _cookies[j - 1] = _cookies[j];
      --_cookieCount;
    } else ++i;
  }
}

void BrowserPersistentState::load() {
  _recentCount = _favoriteCount = _cookieCount = 0;
  bool recentsTrimmed = false;
  bool favoritesTrimmed = false;
  if (!_sdCard.isMounted() || !SpiBusLock::lock(pdMS_TO_TICKS(100))) return;
  File file = SD.open(StatePath, FILE_READ);
  SpiBusLock::unlock();
  if (!file) return;
  while (true) {
    if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) break;
    const bool available = file.available();
    String line = available ? file.readStringUntil('\n') : String();
    SpiBusLock::unlock();
    if (!available) break;
    line.trim();
    if (line.length() < 2 || line[1] != '\t') continue;
    String fields[7]; uint8_t count = 0; int start = 2;
    while (count < 7 && start <= (int)line.length()) {
      int end = line.indexOf('\t', start);
      fields[count++] = line.substring(start, end < 0 ? line.length() : end);
      if (end < 0) break;
      start = end + 1;
    }
    if (line[0] == 'R' && count >= 2) {
      if (_recentCount < BrowserLimits::MaxRecentSites)
        _recentSites[_recentCount++] = {decodeField(fields[0]), decodeField(fields[1])};
      else recentsTrimmed = true;
    } else if (line[0] == 'F' && count >= 2) {
      if (_favoriteCount < BrowserLimits::MaxRecentSites)
        _favoriteSites[_favoriteCount++] = {decodeField(fields[0]), decodeField(fields[1])};
      else favoritesTrimmed = true;
    }
    else if (line[0] == 'C' && count >= 6 && _cookieCount < BrowserLimits::MaxCookies) {
      Browser::Cookie &cookie = _cookies[_cookieCount++];
      cookie = {decodeField(fields[0]), decodeField(fields[1]), decodeField(fields[2]), decodeField(fields[3]),
                fields[4] == "1", strtoull(fields[5].c_str(), nullptr, 10)};
    }
    vTaskDelay(1);
  }
  if (SpiBusLock::lock(pdMS_TO_TICKS(100))) { file.close(); SpiBusLock::unlock(); }
  pruneCookies(nowEpoch());
  if (recentsTrimmed || favoritesTrimmed) save();
}

void BrowserPersistentState::save() {
  pruneCookies(nowEpoch());
  if (!_sdCard.isMounted() || !SpiBusLock::lock(pdMS_TO_TICKS(100))) return;
  if (!SD.exists("/.system") && !SD.mkdir("/.system")) { SpiBusLock::unlock(); return; }
  SD.remove(StateTempPath);
  File file = SD.open(StateTempPath, FILE_WRITE);
  if (!file) { SpiBusLock::unlock(); return; }
  bool written = true;
  for (uint8_t i = 0; i < _recentCount; ++i)
    written = written && file.printf("R\t%s\t%s\n", encodeField(_recentSites[i].url).c_str(), encodeField(_recentSites[i].title).c_str()) > 0;
  for (uint8_t i = 0; i < _favoriteCount; ++i)
    written = written && file.printf("F\t%s\t%s\n", encodeField(_favoriteSites[i].url).c_str(), encodeField(_favoriteSites[i].title).c_str()) > 0;
  for (uint8_t i = 0; i < _cookieCount; ++i) {
    const Browser::Cookie &cookie = _cookies[i];
    written = written && file.printf("C\t%s\t%s\t%s\t%s\t%d\t%llu\n", encodeField(cookie.name).c_str(),
      encodeField(cookie.value).c_str(), encodeField(cookie.domain).c_str(), encodeField(cookie.path).c_str(),
      cookie.secure ? 1 : 0, (unsigned long long)cookie.expiresAt) > 0;
  }
  file.close();
  if (written) { if (SD.exists(StatePath)) SD.remove(StatePath); if (!SD.rename(StateTempPath, StatePath)) SD.remove(StateTempPath); }
  else SD.remove(StateTempPath);
  SpiBusLock::unlock();
  stateLog(written ? F("state committed") : F("state write failed"));
}

String BrowserPersistentState::trimCopy(const String &value) { String result = value; result.trim(); return result; }
String BrowserPersistentState::lower(const String &value) { String result = value; result.toLowerCase(); return result; }

uint64_t BrowserPersistentState::parseHttpDate(const String &source) {
  String value = source; int comma = value.indexOf(','); if (comma >= 0) value = value.substring(comma + 1);
  value.trim(); value.replace("-", " "); int day = 0, year = 0, hour = 0, minute = 0, second = 0; char month[4] = {};
  if (sscanf(value.c_str(), "%d %3s %d %d:%d:%d", &day, month, &year, &hour, &minute, &second) != 6 ||
      year < 1970 || day < 1 || hour > 23 || minute > 59 || second > 59) return 0;
  const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec"; String monthText = lower(month); int monthIndex = -1;
  for (int i = 0; i < 12; ++i) if (lower(String(months).substring(i * 3, i * 3 + 3)) == monthText) { monthIndex = i; break; }
  if (monthIndex < 0) return 0;
  const uint8_t daysInMonth[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  const auto leap = [](int valueYear) { return valueYear % 4 == 0 && (valueYear % 100 != 0 || valueYear % 400 == 0); };
  if (day > daysInMonth[monthIndex] + (monthIndex == 1 && leap(year) ? 1 : 0)) return 0;
  uint64_t days = 0; for (int valueYear = 1970; valueYear < year; ++valueYear) days += leap(valueYear) ? 366 : 365;
  for (int i = 0; i < monthIndex; ++i) days += daysInMonth[i] + (i == 1 && leap(year) ? 1 : 0);
  return (days + day - 1) * 86400ULL + hour * 3600UL + minute * 60UL + second;
}

uint64_t BrowserPersistentState::nowEpoch() {
  const time_t value = time(nullptr);
  return value > 1577836800 ? (uint64_t)value : 0;
}

String BrowserPersistentState::encodeField(const String &value) {
  const char hex[] = "0123456789ABCDEF"; String result;
  for (uint16_t i = 0; i < value.length(); ++i) { const uint8_t byte = (uint8_t)value[i];
    if (isalnum(byte) || byte == '-' || byte == '_' || byte == '.' || byte == '/') result += (char)byte;
    else { result += '%'; result += hex[byte >> 4]; result += hex[byte & 15]; }
  } return result;
}

String BrowserPersistentState::decodeField(const String &value) {
  String result; for (uint16_t i = 0; i < value.length(); ++i) {
    if (value[i] == '%' && i + 2 < value.length()) { char buffer[] = {value[i + 1], value[i + 2], '\0'}; result += (char)strtoul(buffer, nullptr, 16); i += 2; }
    else result += value[i];
  } return result;
}
