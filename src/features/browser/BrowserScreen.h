#pragma once

#include <Arduino.h>

#include "../../media/FileFormats.h"
#include "BrowserDocument.h"
#include "BrowserDownloadStore.h"
#include "BrowserImageWorker.h"
#include "BrowserLifecycleWorker.h"
#include "BrowserLimits.h"
#include "BrowserPageLoader.h"
#include "BrowserPersistentState.h"
#include "BrowserSessionCache.h"
#include "BrowserTransport.h"
#include "BrowserTypes.h"
#include "../../core/Screen.h"

class SdCardController;
class WiFiController;

class BrowserScreen : public Screen,
                      private BrowserLifecycleDelegate {
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
  using PageLoadResult = Browser::PageLoadResult;
  using PageLoadProgress = Browser::PageLoadProgress;

  static constexpr uint8_t MaxRecentSites = BrowserLimits::MaxRecentSites;
  static constexpr uint8_t MaxHistory = BrowserLimits::MaxHistory;
  static constexpr uint32_t MaxBodyBytes = BrowserLimits::MaxBodyBytes;
  static constexpr uint16_t MaxFormPostBytes = BrowserLimits::MaxFormPostBytes;
  static constexpr uint32_t MaxDownloadBytes = BrowserLimits::MaxDownloadBytes;
  static constexpr uint32_t NetworkLockTimeoutMs = BrowserLimits::NetworkLockTimeoutMs;

  BrowserDownloadStore _downloadStore;
  BrowserSessionCache _sessionCache;
  BrowserTransport _transport;
  BrowserPersistentState _persistentState;
  BrowserPageLoader _pageLoader;
  BrowserImageWorker _imageWorker;
  BrowserDocument _document;
  int8_t _editingFormControl;
  String _historyUrls[MaxHistory];
  uint8_t _historyCount;
  bool _loaded;
  BrowserLifecycleWorker _lifecycle;
  bool _showStart;
  bool _showFavorites;
  uint8_t _startListScroll;
  bool _androidUserAgent;
  bool _menuOpen;
  bool _lastTouchDown;
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
  // Downloads open in a child viewer/player.  Keep the source page separate
  // from the downloaded URL so closing that child restores the document
  // rather than a temporary "download opened" page.
  String _downloadReturnUrl;
  uint16_t _nextImageToQueue;
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

  struct PendingNavigation {
    bool active = false;
    bool submitted = false;
    bool hadDocument = false;
    bool addedPreviousPageToHistory = false;
    bool addToRecent = false;
    bool popHistoryOnSuccess = false;
    uint32_t waitStartedMs = 0;
    String previousUrl;
    HttpRequest request;
    Browser::LoadOptions options;
  } _pendingNavigation;

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
  void finishLoading();
  void refreshLoadingDisplay(bool force = false);
  void cancelLoading();
  void startPendingPageLoad(uint32_t nowMs);
  void collectPageLoadProgress();
  void collectPageLoadResult();
  void handlePageLoadResult(PageLoadResult &result);
  void abandonPageLoad();
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
  String resolveUrl(const Url &base, const String &reference) const;

  void renderDocument(const String &html);
  void renderPlainText(const String &text, const String &title);
  void clearRenderedDocumentStorage();
  void drawFormControl(uint16_t rowIndex, int16_t y);
  void drawThumbnail(int16_t imageIndex, int16_t x, int16_t y) const;
  static bool resolveDocumentUrl(void *context, const String &reference, String &resolved);
  void releaseEmbeddedImages();
  void pumpImageJobs();
  void collectImageResults();
  bool imageIsVisibleOnCurrentPage(uint16_t imageIndex) const;
  void startLifecycleLoad();
  void runBrowserLifecycle(uint32_t generation) override;
  void reflowDocument();
  bool hasNextPage() const;
  String shortText(const String &value, int16_t maxWidth) const;
  String lower(const String &value) const;
  int16_t imageWidth(const String &label) const;
  void addHistory(const String &url);
  void addRecent(const String &url, const String &title);

  void loadState();
  void saveState();
};
