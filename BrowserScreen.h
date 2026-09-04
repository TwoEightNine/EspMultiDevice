#pragma once

#include <Arduino.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "FileFormats.h"
#include "BrowserText.h"
#include "src/core/Screen.h"

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
  void onResume() override;
  void onResult(const ScreenResult &result) override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  // Start-page lists are paged in eight-row screens, with room for two pages
  // of independently persisted recent and favorite entries.
  static const uint8_t MaxRecentSites = 16;
  static const uint8_t MaxCookies = 16;
  static const uint16_t MaxRows = 192;
  static const uint8_t MaxRunsPerRow = 16;
  static const uint8_t MaxLinks = 64;
  // Forms are deliberately bounded just like links and image previews. This
  // keeps hostile or generated pages from consuming the ESP32 heap.
  static const uint8_t MaxForms = 8;
  static const uint8_t MaxFormControls = 32;
  static const uint8_t MaxFormOptions = 80;
  static const uint8_t MaxHistory = 8;
  static const uint8_t MaxSessionPages = MaxHistory + 1;
  static const uint8_t MaxSessionImages = 48;
  static const uint8_t MaxStyleDepth = 16;
  static const uint8_t MaxCssRules = 32;
  static const uint8_t MaxCssSelectorLength = 48;
  static const uint16_t MaxCssDeclarationLength = 256;
  static const uint8_t MaxPages = 128;
  // A thumbnail is kept only while there is enough memory for it. The row
  // limit still bounds metadata and placeholders for pathological pages.
  // Retain enough previews for a long article without allowing image-heavy
  // pages to consume the internal heap needed by HTTPS.
  static const uint16_t MaxEmbeddedImages = 48;
  static const uint8_t ImageJobQueueLength = 4;
  static const uint8_t LifecycleJobQueueLength = 1;
  static const uint16_t ImageUrlMaxLength = 384;
  // Keep enough of large legacy pages to reach their body while retaining a
  // bounded allocation on the ESP32.
  static const uint32_t MaxBodyBytes = 512UL * 1024UL;
  static const uint16_t MaxFormPostBytes = 8UL * 1024UL;
  static const uint32_t MaxDownloadBytes = 16UL * 1024UL * 1024UL;
  static const uint32_t MaxSessionCacheBytes = 16UL * 1024UL * 1024UL;
  // Keep the retained part of an HTTP line small.  Some sites send very
  // large informational headers (notably Content-Security-Policy); those
  // can be discarded while continuing to the blank header terminator.
  static const uint16_t MaxHttpLineBytes = 2048;
  static const uint8_t MaxResponseHeaderLines = 64;
  static const uint32_t ConnectTimeoutMs = 10000;
  static const uint8_t TlsRetryHandshakeTimeoutSeconds = 15;
  // A stale background image request must never keep a foreground navigation
  // stuck behind the shared HTTP client.  This is deliberately much shorter
  // than the connection timeout: callers can cancel or retry immediately.
  static const uint32_t NetworkLockTimeoutMs = 2000;
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
    String contentDisposition;
    bool chunked;
    bool compressed;
    int32_t contentLength;
  };

  enum class HttpMethod : uint8_t { Get, Post };

  struct HttpRequest {
    HttpMethod method;
    String url;
    String body;
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
    uint16_t backgroundColor;
    bool hasBackground;
    uint16_t blockBackgroundColor;
    bool hasBlockBackground;
    bool bold;
    bool italic;
    bool underline;
    bool link;
    uint8_t linkIndex;
    bool fullWidthImage;
    BrowserText::Size size = BrowserText::Size::Normal;
  };

  struct StyleOverride {
    bool hasColor;
    uint16_t color;
    bool hasBackground;
    bool backgroundEnabled;
    uint16_t backgroundColor;
    bool hasBold;
    bool bold;
    bool hasItalic;
    bool italic;
    bool hasUnderline;
    bool underline;
    bool hasFullWidthImage;
    bool fullWidthImage;
    bool hasSize;
    BrowserText::Size size;
  };

  struct CssRule {
    String selector;
    StyleOverride style;
    uint16_t sourceOrder;
    uint8_t specificity;
    uint8_t selectorType;
  };

  struct VisualRun {
    bool image;
    String text;
    TextStyle style;
    int16_t imageIndex;
    int16_t x;
    int16_t width;
  };

  struct VisualRow {
    bool image;
    int8_t formControlIndex;
    String imageLabel;
    bool imageLink;
    uint8_t imageLinkIndex;
    int16_t imageIndex;
    TextStyle style;
    int16_t contentWidth;
    uint8_t runCount;
    VisualRun runs[MaxRunsPerRow];
  };

  enum class FormControlKind : uint8_t {
    Text,
    Textarea,
    Checkbox,
    Radio,
    Select,
    Submit,
    Reset,
    Unsupported
  };

  enum class FormMethod : uint8_t { Get, Post, Unsupported };

  struct FormData {
    String action;
    FormMethod method;
    bool urlEncoded;
  };

  struct FormControl {
    FormControlKind kind;
    int8_t formIndex;
    String label;
    bool labelIsPlaceholder;
    String name;
    String id;
    String value;
    String defaultValue;
    bool checked;
    bool defaultChecked;
    bool disabled;
    uint8_t firstOption;
    uint8_t optionCount;
    uint8_t selectedOption;
    uint8_t defaultSelectedOption;
  };

  struct FormOption {
    uint8_t controlIndex;
    String label;
    String value;
  };

  struct EmbeddedImage {
    uint16_t *pixels;
    uint8_t width;
    uint8_t height;
    uint16_t backgroundColor;
    // Image records outlive parsing and worker jobs. Keeping their source as
    // a fixed buffer avoids long-lived Arduino String state in this shared
    // array; a corrupted String here previously crashed the dedupe scan.
    char url[ImageUrlMaxLength];
    uint32_t generation;
    uint8_t state;
  };

  struct ImageJob {
    uint16_t imageIndex;
    uint32_t generation;
    uint16_t backgroundColor;
    bool useSessionCache;
    char url[ImageUrlMaxLength];
  };

  struct SessionPageCacheEntry {
    String url;
    String contentType;
    String path;
    uint32_t bytes;
    uint32_t lastUse;
    bool used;
  };

  struct SessionImageCacheEntry {
    String url;
    String path;
    uint32_t bytes;
    uint32_t lastUse;
    bool used;
  };

  struct ImageResult {
    uint16_t imageIndex;
    uint32_t generation;
    uint16_t *pixels;
    uint8_t width;
    uint8_t height;
  };

  // Startup work is deliberately kept separate from the display task.  A
  // previous implementation emptied the browser cache and parsed saved state
  // from onEnter(), which left the Home screen visible and unresponsive while
  // opening Browser.
  struct LifecycleJob {
    uint32_t generation;
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
  FormData _forms[MaxForms];
  FormControl _formControls[MaxFormControls];
  FormOption _formOptions[MaxFormOptions];
  uint8_t _formCount;
  uint8_t _formControlCount;
  uint8_t _formOptionCount;
  int8_t _editingFormControl;
  String _historyUrls[MaxHistory];
  uint8_t _linkCount;
  uint8_t _historyCount;
  RecentSite _recentSites[MaxRecentSites];
  RecentSite _favoriteSites[MaxRecentSites];
  Cookie _cookies[MaxCookies];
  uint8_t _recentCount;
  uint8_t _favoriteCount;
  uint8_t _cookieCount;
  bool _loaded;
  volatile bool _lifecycleLoading;
  volatile bool _lifecycleDrawPending;
  uint32_t _lifecycleGeneration;
  bool _showStart;
  bool _showFavorites;
  uint8_t _startListScroll;
  bool _androidUserAgent;
  bool _menuOpen;
  bool _lastTouchDown;
  bool _loadCancelled;
  bool _loadCancelTouchDown;
  bool _foregroundLoading;
  uint16_t _page;
  String _currentUrl;
  String _title;
  String _message;
  String _loadingPhase;
  uint32_t _loadingBytes;
  int32_t _loadingTotalBytes;
  uint32_t _loadingStartedMs;
  uint32_t _lastLoadingDrawMs;
  uint32_t _lastLoadingDrawBytes;
  uint8_t _lastLoadingPercent;
  bool _lastLoadingKnownSize;
  volatile bool _sessionCacheActive;
  volatile bool _sessionCacheInitialized;
  bool _usingSessionPage;
  // Downloads open in a child viewer/player.  Keep the source page separate
  // from the downloaded URL so closing that child restores the document
  // rather than a temporary "download opened" page.
  String _downloadReturnUrl;
  bool _deferCachedPageDraw;
  uint32_t _deferCachedPageDrawStartedMs;
  uint32_t _sessionCacheBytes;
  uint32_t _sessionCacheClock;
  uint32_t _sessionCacheSequence;
  SessionPageCacheEntry _sessionPages[MaxSessionPages];
  SessionImageCacheEntry _sessionImages[MaxSessionImages];
  uint16_t _documentBackgroundColor;
  String _titleBuffer;
  VisualRow _rows[MaxRows];
  EmbeddedImage _embeddedImages[MaxEmbeddedImages];
  uint16_t _embeddedImageCount;
  uint16_t _nextImageToQueue;
  uint32_t _imageGeneration;
  volatile uint32_t _activeImageGeneration;
  QueueHandle_t _imageJobs;
  QueueHandle_t _imageResults;
  TaskHandle_t _imageWorker;
  QueueHandle_t _lifecycleJobs;
  TaskHandle_t _lifecycleWorker;
  SemaphoreHandle_t _httpMutex;
  // Session entries are also accessed by the image worker, but reading a
  // cached page must never wait behind that worker's HTTP request.
  SemaphoreHandle_t _sessionCacheMutex;
  uint16_t _rowCount;
  uint16_t _pageStarts[MaxPages];
  uint8_t _pageCount;
  TextStyle _currentStyle;
  StyleFrame _styleStack[MaxStyleDepth];
  uint8_t _styleDepth;
  CssRule _cssRules[MaxCssRules];
  uint8_t _cssRuleCount;
  uint16_t _cssSourceOrder;
  bool _pendingSpace;
  uint16_t _renderedCharacters;
  Rect _closeBounds;
  Rect _backBounds;
  Rect _homeBounds;
  Rect _startAddBounds;
  Rect _startToggleBounds;
  Rect _startPreviousBounds;
  Rect _startNextBounds;
  Rect _previousBounds;
  Rect _nextBounds;
  Rect _startSiteBounds[MaxRecentSites];
  Rect _startRemoveBounds[MaxRecentSites];
  Rect _menuBounds;
  Rect _menuAgentBounds;
  Rect _menuFavoriteBounds;

  void draw();
  void drawHeader();
  void drawStartPage();
  void drawDocument();
  void drawPageMenu();
  void drawLoadingPanel(bool redrawPhase, bool drawBarFrame);
  void redrawDocument();
  void drawControls();
  void drawPageControls();
  void layout();
  bool contains(const Rect &rect, int16_t x, int16_t y) const;
  bool isLoading() const;
  void beginLoading(const String &phase);
  void setLoadingPhase(const String &phase, int32_t totalBytes = -1);
  void advanceLoading(uint32_t bytes);
  void finishLoading();
  void refreshLoadingDisplay(bool force = false);
  void cancelLoading();
  void stopNetworkClientsIfIdle();
  bool pollLoadingCancel();
  String documentLinkAt(int16_t x, int16_t y) const;
  int8_t documentFormControlAt(int16_t x, int16_t y) const;
  void handleFormControl(uint8_t index);
  void openFormTextInput(uint8_t index);
  void resetForm(uint8_t formIndex);
  void submitForm(uint8_t formIndex, int8_t submitControlIndex);
  void openUrlInput();
  bool openUrl(const String &value, bool addToHistory = true, bool addToRecent = false,
               bool useSessionCache = false);
  bool openRequest(const HttpRequest &request, bool addToHistory = true,
                   bool addToRecent = false, bool useSessionCache = false);
  void goBack();
  void toggleUserAgent();
  void toggleFavorite();
  bool isFavorite(const String &url) const;
  void addFavorite(const String &url, const String &title);
  void removeRecent(uint8_t index);
  void removeFavorite(const String &url);
  uint8_t startSiteCount() const;
  const RecentSite &startSiteAt(uint8_t index) const;
  void clampStartListScroll();

  bool normalizeUrl(const String &value, String &normalized, String &error) const;
  bool parseUrl(const String &value, Url &url) const;
  bool fetch(const HttpRequest &initialRequest, String &finalUrl, String &body, String &contentType,
             String &downloadedPath, String &error, uint32_t downloadLimit = MaxDownloadBytes);
  bool requestOne(const Url &url, HttpMethod method, const String &requestBody,
                  HttpResponse &response, WiFiClient *&client, String &error);
  bool readLine(WiFiClient &client, String &line, uint32_t timeoutMs,
                bool discardLongLine = false, bool *lineTruncated = nullptr);
  int readByte(WiFiClient &client, uint32_t timeoutMs);
  bool readBody(WiFiClient &client, bool chunked, int32_t contentLength,
                String &body, String &error);
  bool readDownloadBody(WiFiClient &client, bool chunked, int32_t contentLength,
                        FileFormats::Format format, String &downloadedPath, String &error,
                        uint32_t downloadLimit, const String &suggestedFileName);
  FileFormats::Format downloadableFormatForResponse(const String &contentType, const Url &url) const;
  String downloadFileName(const String &contentDisposition, const Url &url,
                          FileFormats::Format format) const;
  bool isSupportedDownloadFile(const String &path, FileFormats::Format expected) const;
  String resolveUrl(const Url &base, const String &reference) const;
  String cookieHeaderFor(const Url &url) const;
  void acceptCookie(const String &header, const Url &url);
  void pruneCookies();
  uint64_t nowEpoch() const;

  void renderDocument(const String &html);
  void renderPlainText(const String &text, const String &title);
  void clearRenderedDocumentStorage();
  void resetRenderedRows();
  void appendText(const String &text);
  void appendBreak();
  void appendImage(const String &label, const TextStyle &style, bool linked, uint8_t linkIndex,
                   bool fullWidth, int16_t imageIndex = -1);
  void appendStub(const String &label);
  void appendFormControl(uint8_t index, const TextStyle &style);
  int8_t addForm(const String &attributes);
  int8_t addFormControl(FormControlKind kind, int8_t formIndex, const String &attributes,
                        const TextStyle &style, const String &fallbackLabel = String());
  void addSelectOption(uint8_t controlIndex, const String &attributes, const String &text);
  String formControlLabel(const FormControl &control) const;
  String formControlValue(const FormControl &control) const;
  bool buildFormPayload(uint8_t formIndex, int8_t submitControlIndex, String &payload,
                        uint16_t maxBytes = 0) const;
  bool hasAttribute(const String &attributes, const char *name) const;
  String formUrlEncode(const String &value) const;
  void drawFormControl(const VisualRow &row, int16_t y);
  String formStubLabel(const String &tag, const String &attributes) const;
  void appendWord(const String &word, const TextStyle &style);
  void appendRun(const String &text, const TextStyle &style);
  void appendInlineImage(const String &label, const TextStyle &style, int16_t imageIndex);
  void beginTextRow();
  int16_t rowHeight(const VisualRow &row) const;
  void drawThumbnail(int16_t imageIndex, int16_t x, int16_t y) const;
  int16_t fetchEmbeddedImage(const String &source, uint16_t backgroundColor);
  void releaseEmbeddedImages();
  void updateImageLayout();
  void pumpImageJobs();
  void collectImageResults();
  bool cachedPageImagesComplete() const;
  bool imageIsVisibleOnCurrentPage(uint16_t imageIndex) const;
  bool fetchEmbeddedImageOnWorker(const String &url, uint16_t backgroundColor,
                                  bool useSessionCache, uint16_t *&pixels,
                                  uint8_t &width, uint8_t &height);
  static void imageWorkerTask(void *context);
  static void lifecycleWorkerTask(void *context);
  void startLifecycleLoad();
  void buildPages();
  uint16_t pageEndRow(uint8_t page) const;
  bool hasNextPage() const;
  int16_t textWidth(const String &text, const TextStyle &style) const;
  void applyFont(const TextStyle &style) const;
  bool stylesEqual(const TextStyle &left, const TextStyle &right) const;
  TextStyle styledForTag(const String &tag, const String &attributes);
  void collectCssRules(const String &html);
  void parseCssRules(const String &source, uint32_t start, uint32_t end);
  bool cssSlice(const String &source, uint32_t start, uint32_t end, uint16_t limit, String &result) const;
  void skipCssSpaceAndComments(const String &source, uint32_t &position, uint32_t end) const;
  bool addCssRule(const String &selector, const StyleOverride &style, uint16_t sourceOrder);
  bool parseCssDeclarations(const String &declarations, StyleOverride &style) const;
  bool parseBackground(const String &value, uint16_t &color, bool &enabled) const;
  bool matchesCssRule(const CssRule &rule, const String &tag, const String &attributes) const;
  void applyCssRules(TextStyle &style, const String &tag, const String &attributes) const;
  void applyStyleOverride(TextStyle &style, const StyleOverride &override, bool imageTag) const;
  uint8_t registerLink(const String &url);
  String attributeValue(const String &attributes, const char *name) const;
  uint16_t parseColor(const String &value, uint16_t fallback) const;
  bool parseCssColor(const String &value, uint16_t &color) const;
  void applyBackground(TextStyle &style, const String &value) const;
  int findTagEnd(const String &html, int start) const;
  void pushStyle(const String &tag, const TextStyle &style);
  void popStyle(const String &tag);
  String decodeEntities(const String &value) const;
  String shortText(const String &value, int16_t maxWidth) const;
  String shortImageLabel(const String &value, int16_t maxWidth) const;
  String lower(const String &value) const;
  bool isBlockTag(const String &tag) const;
  int16_t imageWidth(const String &label) const;
  void addHistory(const String &url);
  void addRecent(const String &url, const String &title);

  void endSessionCache();
  bool initializeSessionCacheLocked();
  void resetSessionCacheEntries();
  bool clearSessionCacheLocked();
  bool removeSessionFileLocked(const String &path);
  bool ensureSessionCacheSpaceLocked(uint32_t bytes, bool preservePages = false);
  bool evictOldestSessionEntryLocked();
  bool evictOldestSessionImageLocked();
  void removeSessionPageLocked(uint8_t index);
  void removeSessionImageLocked(uint8_t index);
  uint32_t nextSessionCacheUse();
  String nextSessionCachePath(const char *prefix, const char *extension);
  int8_t sessionPageIndex(const String &url) const;
  int8_t sessionImageIndex(const String &url) const;
  bool loadSessionPage(const String &url, String &body, String &contentType);
  bool storeSessionPage(const String &url, const String &contentType, const String &body);
  String sessionImagePath(const String &url);
  void discardSessionImage(const String &url);
  String adoptSessionImage(const String &url, const String &downloadedPath,
                           FileFormats::Format format);

  void loadState();
  void saveState();
  String encodeField(const String &value) const;
  String decodeField(const String &value) const;
};
