#pragma once

#include <Arduino.h>

#include "BrowserTypes.h"

class SdCardController;

// Owns the browser's deliberately plaintext durable state.  This component
// has no UI or navigation dependency and preserves the established on-card
// format exactly.
class BrowserPersistentState {
public:
  explicit BrowserPersistentState(SdCardController &sdCard);

  void load();
  void save();
  // Drops heap-backed copies after Browser closes. Durable state is reloaded
  // from SD on the next session.
  void releaseMemory();

  uint8_t recentCount() const { return _recentCount; }
  uint8_t favoriteCount() const { return _favoriteCount; }
  const Browser::RecentSite &recentAt(uint8_t index) const;
  const Browser::RecentSite &favoriteAt(uint8_t index) const;
  void addRecent(const String &url, const String &title);
  void removeRecent(uint8_t index);
  bool isFavorite(const String &url) const;
  void addFavorite(const String &url, const String &title);
  void removeFavorite(const String &url);

  String cookieHeaderFor(const Browser::Url &url, uint64_t nowEpoch) const;
  void acceptCookie(const String &header, const Browser::Url &url, uint64_t nowEpoch);
  void pruneCookies(uint64_t nowEpoch);

private:
  SdCardController &_sdCard;
  Browser::RecentSite _recentSites[BrowserLimits::MaxRecentSites];
  Browser::RecentSite _favoriteSites[BrowserLimits::MaxRecentSites];
  Browser::Cookie _cookies[BrowserLimits::MaxCookies];
  uint8_t _recentCount;
  uint8_t _favoriteCount;
  uint8_t _cookieCount;

  static String trimCopy(const String &value);
  static String lower(const String &value);
  static uint64_t parseHttpDate(const String &source);
  static uint64_t nowEpoch();
  static String encodeField(const String &value);
  static String decodeField(const String &value);
};
