#pragma once

#include <Arduino.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include "FileFormats.h"
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
  static const uint16_t MaxRows = 192;
  static const uint8_t MaxRunsPerRow = 10;
  static const uint8_t MaxLinks = 64;
  static const uint8_t MaxStyleDepth = 16;
  static const uint8_t MaxPages = 32;
  // Keep enough of large legacy pages to reach their body while retaining a
  // bounded allocation on the ESP32.
  static const uint32_t MaxBodyBytes = 512UL * 1024UL;
  static const uint32_t MaxDownloadBytes = 16UL * 1024UL * 1024UL;
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

  struct HttpResponse {
    int statusCode;
    String location;
    String contentType;
    bool chunked;
    bool compressed;
    int32_t contentLength;
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

  struct TextStyle {
    uint16_t color;
    bool bold;
    bool italic;
    bool underline;
    bool link;
    uint8_t linkIndex;
  };

  struct VisualRun {
    String text;
    TextStyle style;
    int16_t x;
    int16_t width;
  };

  struct VisualRow {
    bool image;
    String imageLabel;
    bool imageLink;
    uint8_t imageLinkIndex;
    int16_t contentWidth;
    uint8_t runCount;
    VisualRun runs[MaxRunsPerRow];
  };

  struct StyleFrame {
    String tag;
    TextStyle style;
  };

  WiFiController &_wifi;
  SdCardController &_sdCard;
  WiFiClient _plainClient;
  WiFiClientSecure _secureClient;
  String _linkUrls[MaxLinks];
  uint8_t _linkCount;
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
  String _titleBuffer;
  VisualRow _rows[MaxRows];
  uint16_t _rowCount;
  uint16_t _pageStarts[MaxPages];
  uint8_t _pageCount;
  TextStyle _currentStyle;
  StyleFrame _styleStack[MaxStyleDepth];
  uint8_t _styleDepth;
  bool _pendingSpace;
  uint16_t _renderedCharacters;
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
  String documentLinkAt(int16_t x, int16_t y) const;
  void openUrlInput();
  void openUrl(const String &value);

  bool normalizeUrl(const String &value, String &normalized, String &error) const;
  bool parseUrl(const String &value, Url &url) const;
  bool fetch(const String &initialUrl, String &finalUrl, String &body, String &contentType,
             String &downloadedPath, String &error);
  bool requestOne(const Url &url, HttpResponse &response, WiFiClient *&client, String &error);
  bool readLine(WiFiClient &client, String &line, uint32_t timeoutMs) const;
  int readByte(WiFiClient &client, uint32_t timeoutMs) const;
  bool readBody(WiFiClient &client, bool chunked, int32_t contentLength,
                String &body, String &error) const;
  bool readDownloadBody(WiFiClient &client, bool chunked, int32_t contentLength,
                        FileFormats::Format format, String &downloadedPath, String &error);
  FileFormats::Format downloadableFormatForResponse(const String &contentType, const Url &url) const;
  bool isSupportedDownloadFile(const String &path, FileFormats::Format expected) const;
  String resolveUrl(const Url &base, const String &reference) const;
  String cookieHeaderFor(const Url &url) const;
  void acceptCookie(const String &header, const Url &url);
  void pruneCookies();
  uint64_t nowEpoch() const;

  void renderDocument(const String &html);
  void renderPlainText(const String &text, const String &title);
  void resetRenderedRows();
  void appendText(const String &text);
  void appendBreak();
  void appendImage(const String &label, bool linked, uint8_t linkIndex);
  void appendStub(const String &label);
  void appendWord(const String &word, const TextStyle &style);
  void appendRun(const String &text, const TextStyle &style);
  void beginTextRow();
  void buildPages();
  uint16_t pageEndRow(uint8_t page) const;
  bool hasNextPage() const;
  int16_t textWidth(const String &text, const TextStyle &style) const;
  void applyFont(const TextStyle &style) const;
  bool stylesEqual(const TextStyle &left, const TextStyle &right) const;
  TextStyle styledForTag(const String &tag, const String &attributes);
  uint8_t registerLink(const String &url);
  String attributeValue(const String &attributes, const char *name) const;
  uint16_t parseColor(const String &value, uint16_t fallback) const;
  int findTagEnd(const String &html, int start) const;
  void pushStyle(const String &tag, const TextStyle &style);
  void popStyle(const String &tag);
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
