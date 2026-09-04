#pragma once

#include <Arduino.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "../../media/FileFormats.h"
#include "BrowserLimits.h"
#include "BrowserPersistentState.h"
#include "BrowserLifecycleWorker.h"
#include "BrowserDocument.h"
#include "BrowserSessionCache.h"
#include "BrowserTransport.h"
#include "BrowserTypes.h"
#include "../../core/Screen.h"

class SdCardController;
class WiFiController;

class BrowserScreen : public Screen, private BrowserSessionCache, private BrowserLifecycleDelegate {
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
  using Rect = Browser::Rect;
  using Url = Browser::Url;
  using HttpResponse = Browser::HttpResponse;
  using HttpMethod = Browser::HttpMethod;
  using HttpRequest = Browser::HttpRequest;
  using RecentSite = Browser::RecentSite;
  using VisualRun = Browser::VisualRun;
  using VisualRow = Browser::VisualRow;
  using FormControlKind = Browser::FormControlKind;
  using FormMethod = Browser::FormMethod;
  using FormData = Browser::FormData;
  using FormControl = Browser::FormControl;
  using EmbeddedImage = Browser::EmbeddedImage;
  using ImageJob = Browser::ImageJob;
  using ImageResult = Browser::ImageResult;
  using SessionPageCacheEntry = Browser::SessionPageCacheEntry;
  using SessionImageCacheEntry = Browser::SessionImageCacheEntry;

  static constexpr uint8_t MaxRecentSites = BrowserLimits::MaxRecentSites;
  static constexpr uint8_t MaxHistory = BrowserLimits::MaxHistory;
  static constexpr uint8_t MaxSessionPages = BrowserLimits::MaxSessionPages;
  static constexpr uint8_t MaxSessionImages = BrowserLimits::MaxSessionImages;
  static constexpr uint8_t ImageJobQueueLength = BrowserLimits::ImageJobQueueLength;
  static constexpr uint32_t MaxBodyBytes = BrowserLimits::MaxBodyBytes;
  static constexpr uint16_t MaxFormPostBytes = BrowserLimits::MaxFormPostBytes;
  static constexpr uint32_t MaxDownloadBytes = BrowserLimits::MaxDownloadBytes;
  static constexpr uint32_t MaxSessionCacheBytes = BrowserLimits::MaxSessionCacheBytes;
  static constexpr uint16_t MaxHttpLineBytes = BrowserLimits::MaxHttpLineBytes;
  static constexpr uint8_t MaxResponseHeaderLines = BrowserLimits::MaxResponseHeaderLines;
  static constexpr uint32_t ConnectTimeoutMs = BrowserLimits::ConnectTimeoutMs;
  static constexpr uint8_t TlsRetryHandshakeTimeoutSeconds = BrowserLimits::TlsRetryHandshakeTimeoutSeconds;
  static constexpr uint32_t NetworkLockTimeoutMs = BrowserLimits::NetworkLockTimeoutMs;
  static constexpr uint8_t MaxRedirects = BrowserLimits::MaxRedirects;

  WiFiController &_wifi;
  SdCardController &_sdCard;
  BrowserTransport _transport;
  BrowserDocument _document;
  int8_t _editingFormControl;
  String _historyUrls[MaxHistory];
  uint8_t _historyCount;
  BrowserPersistentState _persistentState;
  bool _loaded;
  BrowserLifecycleWorker _lifecycle;
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
  bool _usingSessionPage;
  // Downloads open in a child viewer/player.  Keep the source page separate
  // from the downloaded URL so closing that child restores the document
  // rather than a temporary "download opened" page.
  String _downloadReturnUrl;
  bool _deferCachedPageDraw;
  uint32_t _deferCachedPageDrawStartedMs;
  uint16_t _nextImageToQueue;
  uint32_t _imageGeneration;
  volatile uint32_t _activeImageGeneration;
  QueueHandle_t _imageJobs;
  QueueHandle_t _imageResults;
  TaskHandle_t _imageWorker;
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
  BrowserDocumentLayout documentLayout() const;
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
  void drawFormControl(uint16_t rowIndex, int16_t y);
  void drawThumbnail(int16_t imageIndex, int16_t x, int16_t y) const;
  static bool resolveDocumentUrl(void *context, const String &reference, String &resolved);
  void releaseEmbeddedImages();
  void pumpImageJobs();
  void collectImageResults();
  bool cachedPageImagesComplete() const;
  bool imageIsVisibleOnCurrentPage(uint16_t imageIndex) const;
  bool fetchEmbeddedImageOnWorker(const String &url, uint16_t backgroundColor,
                                  bool useSessionCache, uint16_t *&pixels,
                                  uint8_t &width, uint8_t &height);
  static void imageWorkerTask(void *context);
  void startLifecycleLoad();
  void runBrowserLifecycle(uint32_t generation) override;
  void reflowDocument();
  bool hasNextPage() const;
  String shortText(const String &value, int16_t maxWidth) const;
  String lower(const String &value) const;
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
};
