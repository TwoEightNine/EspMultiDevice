#pragma once

#include <Arduino.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include "Screen.h"

class SdCardController;
class WiFiController;

class BrowserScreen : public Screen {
public:
  BrowserScreen(Navigator &navigator,
                TouchController &touch,
                DisplayController &display,
                WiFiController &wifi,
                SdCardController &sdCard);

  void onEnter() override;
  void onResult(const ScreenResult &result) override;
  void handleTouch(const TouchPoint &point) override;

private:
  static const uint8_t MaxRecentSites = 8;
  static const uint8_t MaxCookies = 16;
  static const uint16_t MaxLines = 180;
  // Keep enough of large legacy pages to reach their body while retaining a
  // bounded allocation on the ESP32.
  static const uint32_t MaxBodyBytes = 512UL * 1024UL;
  static const uint32_t ConnectTimeoutMs = 10000;
  static const uint8_t MaxRedirects = 5;

  struct Rect {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
  };

  struct Url {
    String scheme;
    String host;
    String path;
    uint16_t port;
  };

  struct RecentSite {
    String url;
    String title;
  };

  struct Cookie {
    String name;
    String value;
    String domain;
    String path;
    bool secure;
    uint64_t expiresAt;
  };

  WiFiController &_wifi;
  SdCardController &_sdCard;
  WiFiClient _plainClient;
  WiFiClientSecure _secureClient;
  RecentSite _recentSites[MaxRecentSites];
  Cookie _cookies[MaxCookies];
  uint8_t _recentCount;
  uint8_t _cookieCount;
  bool _loaded;
  bool _showStart;
  bool _lastTouchDown;
  uint16_t _page;
  String _currentUrl;
  String _title;
  String _message;
  String _renderBuffer;
  String _titleBuffer;
  String _lines[MaxLines];
  uint16_t _lineCount;
  Rect _closeBounds;
  Rect _startBounds;
  Rect _newBounds;
  Rect _openBounds;
  Rect _previousBounds;
  Rect _nextBounds;
  Rect _recentBounds[MaxRecentSites];

  void draw();
  void drawHeader();
  void drawStartPage();
  void drawDocument();
  void drawControls();
  void layout();
  bool contains(const Rect &rect, int16_t x, int16_t y) const;
  void openUrlInput();
  void openUrl(const String &value);

  bool normalizeUrl(const String &value, String &normalized, String &error) const;
  bool parseUrl(const String &value, Url &url) const;
  bool fetch(const String &initialUrl, String &finalUrl, String &body, String &contentType, String &error);
  bool fetchOne(const Url &url, int &statusCode, String &location, String &body,
                String &contentType, String &error);
  bool readLine(WiFiClient &client, String &line, uint32_t timeoutMs) const;
  int readByte(WiFiClient &client, uint32_t timeoutMs) const;
  bool readBody(WiFiClient &client, bool chunked, int32_t contentLength,
                String &body, String &error) const;
  String resolveRedirect(const Url &base, const String &location) const;
  String cookieHeaderFor(const Url &url) const;
  void acceptCookie(const String &header, const Url &url);
  void pruneCookies();
  uint64_t nowEpoch() const;

  void renderDocument(const String &html);
  void appendText(const String &text);
  void appendBreak();
  void appendStub(const String &label);
  void wrapRenderedText(const String &text);
  String decodeEntities(const String &value) const;
  String transliterateCyrillic(const String &value) const;
  String shortText(const String &value, int16_t maxWidth) const;
  String lower(const String &value) const;
  bool isBlockTag(const String &tag) const;
  void addRecent(const String &url, const String &title);

  void loadState();
  void saveState();
  String encodeField(const String &value) const;
  String decodeField(const String &value) const;
};
