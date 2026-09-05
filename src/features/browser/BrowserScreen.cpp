#include "BrowserScreen.h"

#include "../../app/Navigator.h"
#include "BrowserText.h"
#include "../../services/storage/SdCardController.h"
#include "../../core/SpiBusLock.h"
#include "../../ui/UiKit.h"
#include "../../services/network/WiFiController.h"
#include "../../ui/icons.h"

namespace {
const int16_t HeaderHeight = UiKit::AppBarHeight;
const int16_t ControlsHeight = 42;
const int16_t TextRowHeight = 22;
const int16_t FormControlHeight = 42;
const int16_t FormTextareaHeight = 58;
const int16_t DocumentPadding = 7;
const int16_t DocumentSidePadding = 8;
const uint8_t StartListVisibleRows = 8;
const int16_t RecentListTop = HeaderHeight + 24;
const uint16_t FooterButtonColor = 0x2965;
const uint16_t DefaultTextColor = TFT_BLACK;
const uint16_t ImagePlaceholderColor = TFT_DARKGREY;
const uint16_t LinkImageColor = TFT_BLUE;
const char *ImagePlaceholderLabel = "image";
// Some file hosts route unknown clients to an informational page instead of
// returning the requested media. The menu can temporarily present this
// conventional mobile identity so servers use their normal mobile response.
const char *EspUserAgent = "ESP-Media-Browser/1.0";
const char *MobileUserAgent =
  "Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 "
  "(KHTML, like Gecko) Chrome/120.0.0.0 Mobile Safari/537.36";

void browserLog(const __FlashStringHelper *message) {
  Serial.print(F("[Browser] "));
  Serial.println(message);
}

void browserLog(const String &message) {
  Serial.print(F("[Browser] "));
  Serial.println(message);
}

// The TFT, SD card, and image decoders share the SPI bus.  Image decoding
// runs on the other core, so every Browser display transaction must wait for
// the same lock used by SD operations before touching the TFT.
class BrowserDisplayLock {
public:
  BrowserDisplayLock() : _locked(SpiBusLock::lock(pdMS_TO_TICKS(100))) {}
  ~BrowserDisplayLock() { if (_locked) SpiBusLock::unlock(); }
  bool locked() const { return _locked; }

private:
  bool _locked;
};

uint8_t utf8CharacterBytes(const String &value, uint16_t offset) {
  if (offset >= value.length()) return 0;
  const uint8_t first = (uint8_t)value[offset];
  uint8_t length = first < 0x80 ? 1 : (first >= 0xc2 && first <= 0xdf ? 2 :
                   (first >= 0xe0 && first <= 0xef ? 3 : (first >= 0xf0 && first <= 0xf4 ? 4 : 1)));
  if (offset + length > value.length()) return 1;
  for (uint8_t i = 1; i < length; ++i) if (((uint8_t)value[offset + i] & 0xc0) != 0x80) return 1;
  return length;
}

void drawCheckbox(TFT_eSPI &tft, int16_t x, int16_t y, bool checked, uint16_t color) {
  tft.drawRect(x, y, 16, 16, color);
  if (!checked) return;
  tft.drawLine(x + 3, y + 8, x + 7, y + 12, color);
  tft.drawLine(x + 7, y + 12, x + 13, y + 4, color);
  tft.drawLine(x + 3, y + 9, x + 7, y + 13, color);
  tft.drawLine(x + 7, y + 13, x + 14, y + 4, color);
}

}

BrowserScreen::BrowserScreen(Navigator &navigator,
                             TouchController &touch,
                             DisplayController &display,
                             WiFiController &wifi,
                             SdCardController &sdCard)
  : Screen(navigator, touch, display),
    _downloadStore(sdCard),
    _sessionCache(sdCard),
    _transport(wifi, _downloadStore),
    _persistentState(sdCard),
    _pageLoader(_transport, _sessionCache, _persistentState),
    _imageWorker(_transport, _sessionCache, _persistentState),
    _editingFormControl(-1),
    _historyCount(0),
    _loaded(false),
    _lifecycle(*this),
    _showStart(true),
    _showFavorites(false),
    _startListScroll(0),
    _androidUserAgent(false),
    _menuOpen(false),
    _lastTouchDown(false),
    _foregroundLoading(false),
    _page(0),
    _currentUrl(),
    _title(),
    _message(),
    _loadingPhase(),
    _loadingBytes(0),
    _loadingTotalBytes(-1),
    _loadingStartedMs(0),
    _lastLoadingDrawMs(0),
    _lastLoadingDrawBytes(0),
    _lastLoadingPercent(0),
    _lastLoadingKnownSize(false),
    _downloadReturnUrl(),
    _nextImageToQueue(0),
    _closeBounds{0, 0, UiKit::AppBarHeight, UiKit::AppBarHeight},
    _backBounds{0, 0, 0, 0},
    _homeBounds{0, 0, 0, 0},
    _startAddBounds{0, 0, 0, 0},
    _startToggleBounds{0, 0, 0, 0},
    _startPreviousBounds{0, 0, 0, 0},
    _startNextBounds{0, 0, 0, 0},
    _previousBounds{0, 0, 0, 0},
    _nextBounds{0, 0, 0, 0},
    _menuBounds{0, 0, 0, 0},
    _menuAgentBounds{0, 0, 0, 0},
    _menuFavoriteBounds{0, 0, 0, 0} {
  for (uint8_t i = 0; i < MaxRecentSites; i++) {
    _startSiteBounds[i] = {0, 0, 0, 0};
    _startRemoveBounds[i] = {0, 0, 0, 0};
  }
  _pageLoader.begin();
  _imageWorker.begin();
  {
#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
    const BaseType_t workerCore = 0;
#else
    const BaseType_t workerCore = ARDUINO_RUNNING_CORE == 0 ? 1 : 0;
#endif
    // This worker only prepares disposable cache data and saved state.  It
    // must never outrun the core's idle task: on the usual Arduino dual-core
    // setup it is pinned to CPU 0, whose idle task feeds the task watchdog.
    if (!_lifecycle.begin(workerCore)) browserLog(F("browser startup worker unavailable"));
  }
}

void BrowserScreen::onEnter() {
  browserLog(F("entered"));
  _lastTouchDown = false;
  // Draw the interactive shell before any SD work.  The lifecycle worker
  // loads persistent state and clears the previous session cache on the
  // other core; this avoids holding the Home screen during Browser entry.
  if ((!_sessionCache.active() || !_loaded) && !_lifecycle.loading()) startLifecycleLoad();
  draw();
}

void BrowserScreen::onResume() {
  _lastTouchDown = false;
  if (_downloadReturnUrl.length() == 0) {
    onEnter();
    return;
  }

  const String sourceUrl = _downloadReturnUrl;
  _downloadReturnUrl = "";
  browserLog(String(F("restoring download source from session cache: ")) + sourceUrl);
  // A download is not a browser page.  Reopen the document that initiated it
  // without changing history; openUrl() uses the retained session copy first
  // and only falls back to the network if that copy is unavailable.
  if (!openUrl(sourceUrl, false, false, true)) {
    browserLog(F("could not restore download source"));
  }
}

void BrowserScreen::onResult(const ScreenResult &result) {
  if (_editingFormControl >= 0) {
    if (result.kind == ScreenResultKind::Text &&
        _editingFormControl < _document.formControlCount()) {
      if (_document.setFormValue(_editingFormControl, result.text)) draw();
    }
    _editingFormControl = -1;
    return;
  }
  if (result.kind == ScreenResultKind::Text && result.text.length() > 0) {
    browserLog(F("received URL from text input"));
    openUrl(result.text, true, true);
  }
}

void BrowserScreen::tick(uint32_t nowMs) {
  if (_lifecycle.takeDrawPending()) {
    draw();
  }
  startPendingPageLoad(nowMs);
  collectPageLoadProgress();
  collectPageLoadResult();
  if (_navigator.activeScreen() != this) return;
  collectImageResults();
  pumpImageJobs();
}

void BrowserScreen::startLifecycleLoad() {
  // User-agent override is intentionally session-only.  Starting Browser
  // after it has been closed always returns to the lightweight ESP identity.
  _androidUserAgent = false;
  _showFavorites = false;
  _startListScroll = 0;
  _menuOpen = false;
  // Do not touch the SD card here.  A failed queue allocation still leaves a
  // useful Browser shell; state can be loaded on a later entry.
  if (!_lifecycle.schedule()) browserLog(F("browser startup job could not be queued"));
}

void BrowserScreen::runBrowserLifecycle(uint32_t generation) {
  // A browser session never needs its old temporary pages after it has been
  // closed. This method runs only on BrowserLifecycleWorker's other-core task.
  _sessionCache.beginSession();
  if (_lifecycle.isCurrent(generation)) _sessionCache.prepareSession();
  if (_lifecycle.isCurrent(generation) && !_loaded) {
    // Let IDLE0 run between cleanup and state parsing on the Arduino core.
    vTaskDelay(1);
    loadState();
    _loaded = true;
  }
  if (_lifecycle.isCurrent(generation)) browserLog(F("browser startup work complete"));
}

void BrowserScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) {
    _lastTouchDown = false;
    return;
  }
  if (_lastTouchDown) return;
  _lastTouchDown = true;

  if (_menuOpen) {
    if (contains(_menuAgentBounds, point.x, point.y)) {
      toggleUserAgent();
      return;
    }
    if (contains(_menuFavoriteBounds, point.x, point.y)) {
      toggleFavorite();
      return;
    }
    _menuOpen = false;
    draw();
    return;
  }

  if (contains(_closeBounds, point.x, point.y)) {
    browserLog(F("close requested"));
    abandonPageLoad();
    releaseEmbeddedImages();
    // Never wait for an image decoder or delete a directory from the touch
    // handler.  The next Browser entry clears the disposable session cache
    // from its lifecycle worker.
    _lifecycle.cancel();
    _sessionCache.endSession();
    browserLog(F("session cache cleanup deferred"));
    _historyCount = 0;
    _currentUrl = "";
    _downloadReturnUrl = "";
    _title = "";
    _message = "";
    _document.clear();
    _page = 0;
    _showStart = true;
    _showFavorites = false;
    _startListScroll = 0;
    _menuOpen = false;
    _navigator.pop();
    return;
  }
  // Foreground page loading is asynchronous, so explicitly gate controls
  // that could previously never run while openRequest() blocked this task.
  if (_foregroundLoading) {
    if (!_showStart && contains(_backBounds, point.x, point.y)) {
      browserLog(F("cancel requested"));
      cancelLoading();
    }
    return;
  }
  if (!_showStart && contains(_menuBounds, point.x, point.y)) {
    _menuOpen = true;
    draw();
    return;
  }
  if (!_showStart && contains(_backBounds, point.x, point.y)) {
    if (isLoading()) {
      browserLog(F("cancel requested"));
      cancelLoading();
    } else if (!_showStart) {
      browserLog(F("back requested"));
      goBack();
    }
    return;
  }
  if (!_showStart && contains(_homeBounds, point.x, point.y)) {
    browserLog(F("returning to start page"));
    if (isLoading()) releaseEmbeddedImages();
    _showStart = true;
    _page = 0;
    _message = "";
    _showFavorites = false;
    _startListScroll = 0;
    _menuOpen = false;
    draw();
    return;
  }
  if (_showStart && contains(_startAddBounds, point.x, point.y)) {
    browserLog(F("opening URL input"));
    openUrlInput();
    return;
  }
  if (_showStart && contains(_startToggleBounds, point.x, point.y)) {
    _showFavorites = !_showFavorites;
    _startListScroll = 0;
    browserLog(_showFavorites ? F("showing favorite sites") : F("showing recent sites"));
    draw();
    return;
  }
  if (_showStart && !_lifecycle.loading() && contains(_startPreviousBounds, point.x, point.y)) {
    if (_startListScroll > 0) {
      _startListScroll -= StartListVisibleRows;
      draw();
    }
    return;
  }
  if (_showStart && !_lifecycle.loading() && contains(_startNextBounds, point.x, point.y)) {
    const uint8_t count = startSiteCount();
    if (_startListScroll + StartListVisibleRows < count) {
      _startListScroll += StartListVisibleRows;
      draw();
    }
    return;
  }
  if (!_showStart && contains(_previousBounds, point.x, point.y)) {
    if (_page > 0) {
      _page--;
      browserLog(String(F("moved to page ")) + String(_page + 1));
      redrawDocument();
      drawPageControls();
    }
    return;
  }
  if (!_showStart && contains(_nextBounds, point.x, point.y)) {
    if (hasNextPage()) {
      _page++;
      browserLog(String(F("moved to page ")) + String(_page + 1));
      redrawDocument();
      drawPageControls();
    }
    return;
  }
  if (_showStart) {
    for (uint8_t i = 0; i < MaxRecentSites; i++) {
      if (contains(_startSiteBounds[i], point.x, point.y)) {
        const RecentSite &site = startSiteAt(i);
        if (contains(_startRemoveBounds[i], point.x, point.y)) {
          if (_showFavorites) {
            browserLog(String(F("removing favorite site #")) + String(i + 1));
            removeFavorite(site.url);
          } else {
            browserLog(String(F("removing recent site #")) + String(i + 1));
            removeRecent(i);
          }
          saveState();
          draw();
          return;
        }
        browserLog(String(_showFavorites ? F("opening favorite site #") : F("opening recent site #")) + String(i + 1));
        const String url = site.url;
        const String title = site.title;
        if (!_showFavorites) {
          // Promote immediately so the tap is reflected even if the reload
          // fails; a successful load below refreshes this title from the page.
          addRecent(url, title);
          saveState();
        }
        openUrl(url, true, !_showFavorites);
        return;
      }
    }
  }
  if (!_showStart) {
    int8_t formControl = documentFormControlAt(point.x, point.y);
    if (formControl >= 0) {
      handleFormControl((uint8_t)formControl);
      return;
    }
    String linkUrl = documentLinkAt(point.x, point.y);
    if (linkUrl.length()) {
      browserLog(String(F("link tapped: current=")) + _currentUrl + F(", target=") + linkUrl +
                 F(", targetIsCurrent=") + (linkUrl == _currentUrl ? F("yes") : F("no")));
      openUrl(linkUrl);
    }
  }
}

void BrowserScreen::layout() {
  const int16_t width = _display.width();
  const int16_t height = _display.height();
  const int16_t buttonWidth = width / 4;
  _closeBounds = {0, 0, UiKit::AppBarHeight, UiKit::AppBarHeight};
  _backBounds = {0, (int16_t)(height - ControlsHeight), buttonWidth, ControlsHeight};
  _homeBounds = {buttonWidth, (int16_t)(height - ControlsHeight), buttonWidth, ControlsHeight};
  _startAddBounds = {0, (int16_t)(height - ControlsHeight), buttonWidth, ControlsHeight};
  _startToggleBounds = {buttonWidth, (int16_t)(height - ControlsHeight), buttonWidth, ControlsHeight};
  _startPreviousBounds = {(int16_t)(buttonWidth * 2), (int16_t)(height - ControlsHeight), buttonWidth, ControlsHeight};
  _startNextBounds = {(int16_t)(buttonWidth * 3), (int16_t)(height - ControlsHeight),
                      (int16_t)(width - buttonWidth * 3), ControlsHeight};
  _previousBounds = {(int16_t)(buttonWidth * 2), (int16_t)(height - ControlsHeight), buttonWidth, ControlsHeight};
  _nextBounds = {(int16_t)(buttonWidth * 3), (int16_t)(height - ControlsHeight),
                 (int16_t)(width - buttonWidth * 3), ControlsHeight};
  for (uint8_t i = 0; i < MaxRecentSites; i++) {
    _startSiteBounds[i] = {0, 0, 0, 0};
    _startRemoveBounds[i] = {0, 0, 0, 0};
  }
  clampStartListScroll();
  const uint8_t count = _lifecycle.loading() ? 0 : startSiteCount();
  const uint8_t visible = count - _startListScroll < StartListVisibleRows
    ? count - _startListScroll : StartListVisibleRows;
  for (uint8_t row = 0; row < visible; row++) {
    const uint8_t index = _startListScroll + row;
    _startSiteBounds[index] = {0, (int16_t)(RecentListTop + row * UiKit::ListCellHeight),
                        width, UiKit::ListCellHeight};
    _startRemoveBounds[index] = {(int16_t)(width - UiKit::ListCellHeight),
                                 (int16_t)(RecentListTop + row * UiKit::ListCellHeight),
                                 UiKit::ListCellHeight, UiKit::ListCellHeight};
  }
  _menuBounds = {(int16_t)(width - UiKit::NavigationIconSize - UiKit::AppBarIconPadding),
                 UiKit::AppBarIconPadding,
                 UiKit::NavigationIconSize, UiKit::NavigationIconSize};
  const int16_t menuWidth = width > 240 ? 224 : width - 16;
  const int16_t menuX = width - menuWidth - 8;
  _menuAgentBounds = {menuX, (int16_t)(HeaderHeight + 6), menuWidth, UiKit::ListCellHeight};
  _menuFavoriteBounds = {menuX, (int16_t)(HeaderHeight + 6 + UiKit::ListCellHeight),
                         menuWidth, UiKit::ListCellHeight};
}

BrowserDocumentLayout BrowserScreen::documentLayout() const {
  return {
    _display.width(),
    static_cast<int16_t>(HeaderHeight + DocumentPadding),
    static_cast<int16_t>(_display.height() - ControlsHeight - 4),
    DocumentSidePadding,
    TextRowHeight,
    FormControlHeight,
    FormTextareaHeight,
    imageWidth("")
  };
}

void BrowserScreen::draw() {
  if (!_display.isReady()) return;
  BrowserDisplayLock lock;
  if (!lock.locked()) {
    browserLog(F("display update deferred: SPI bus is busy"));
    return;
  }
  layout();
  TFT_eSPI &tft = _display.tft();
  BrowserText::begin(tft);
  tft.fillScreen(TFT_BLACK);
  drawHeader();
  if (_showStart) drawStartPage();
  else drawDocument();
  drawControls();
  if (_menuOpen && !_showStart) drawPageMenu();
  BrowserText::end(tft);
}

void BrowserScreen::drawHeader() {
  String heading = _showStart ? "Browser" : (_title.length() ? _title : _currentUrl);
  TFT_eSPI &tft = _display.tft();
  UiKit::drawTopAppBar(tft, _display.width(), "");
  const int16_t titleWidth = _display.width() - (_showStart ? 72 : 120);
  BrowserText::draw(tft, BrowserText::ellipsize(tft, heading, titleWidth,
                                                 BrowserText::Size::Title),
                    UiKit::AppBarHeight, 9, TFT_WHITE,
                    UiKit::AppBarColor, BrowserText::Size::Title);
  if (!_showStart) {
    UiKit::drawMoreIcon(tft, _menuBounds.x + _menuBounds.w / 2,
                        _menuBounds.y + _menuBounds.h / 2,
                        TFT_WHITE, UiKit::AppBarColor);
  }
}

void BrowserScreen::drawStartPage() {
  TFT_eSPI &tft = _display.tft();
  UiText::draw(tft, _showFavorites ? "Favorite sites" : "Recent sites", 10, RecentListTop - 18, TFT_LIGHTGREY, TFT_BLACK,
               UiText::Size::Compact);
  // Recent-site records are populated by the lifecycle worker.  The footer
  // remains usable while it works, but the list itself is read only after its
  // hand-off frame in tick().
  if (_lifecycle.loading()) {
    UiText::draw(tft, "Loading saved browser data...", 10, RecentListTop + 10,
                 TFT_DARKGREY, TFT_BLACK, UiText::Size::Compact);
    return;
  }
  const uint8_t count = startSiteCount();
  if (count == 0) {
    UiText::draw(tft, _showFavorites ? "No favorite sites yet" : "No recent sites yet", 10, RecentListTop + 10,
                 TFT_DARKGREY, TFT_BLACK, UiText::Size::Compact);
    return;
  }
  for (uint8_t i = 0; i < MaxRecentSites; i++) {
    if (_startSiteBounds[i].w == 0) continue;
    const RecentSite &site = startSiteAt(i);
    const Rect &bounds = _startSiteBounds[i];
    const Rect &removeBounds = _startRemoveBounds[i];
    String label = site.title.length() ? site.title : site.url;
    tft.fillRect(0, bounds.y, bounds.w, UiKit::ListCellHeight, TFT_BLACK);
    BrowserText::draw(tft, BrowserText::ellipsize(tft, label, bounds.w - UiKit::ContentPadding - removeBounds.w - 6),
                      UiKit::ContentPadding, bounds.y + 10, TFT_WHITE, TFT_BLACK,
                      BrowserText::Size::Content);
    UiKit::drawListCloseIcon(tft, removeBounds.x + removeBounds.w / 2,
                             removeBounds.y + removeBounds.h / 2,
                             TFT_LIGHTGREY, TFT_BLACK);
    tft.drawFastHLine(UiKit::ContentPadding, bounds.y + UiKit::ListCellHeight - 1,
                      bounds.w - UiKit::ContentPadding * 2, UiKit::DividerColor);
  }
}

void BrowserScreen::drawPageMenu() {
  TFT_eSPI &tft = _display.tft();
  const int16_t menuX = _menuAgentBounds.x;
  const int16_t menuY = _menuAgentBounds.y;
  const int16_t menuWidth = _menuAgentBounds.w;
  const int16_t menuHeight = _menuAgentBounds.h + _menuFavoriteBounds.h;
  tft.fillRoundRect(menuX, menuY, menuWidth, menuHeight, 4, TFT_BLACK);
  tft.drawRoundRect(menuX, menuY, menuWidth, menuHeight, 4, TFT_DARKGREY);
  UiText::draw(tft, "Override User-Agent", menuX + UiKit::ContentPadding,
               _menuAgentBounds.y + UiKit::ListCellHeight / 2, TFT_WHITE, TFT_BLACK,
               UiText::Size::Compact, ML_DATUM);
  drawCheckbox(tft, menuX + menuWidth - UiKit::ContentPadding - 16,
               _menuAgentBounds.y + (_menuAgentBounds.h - 16) / 2,
               _androidUserAgent, TFT_WHITE);
  tft.drawFastHLine(menuX + UiKit::ContentPadding, _menuFavoriteBounds.y,
                    menuWidth - UiKit::ContentPadding * 2, UiKit::DividerColor);
  UiText::draw(tft, isFavorite(_currentUrl) ? "Remove from favorites" : "Add to favorites",
               menuX + UiKit::ContentPadding, _menuFavoriteBounds.y + UiKit::ListCellHeight / 2,
               TFT_WHITE, TFT_BLACK, UiText::Size::Compact, ML_DATUM);
}

void BrowserScreen::drawDocument() {
  TFT_eSPI &tft = _display.tft();
  const uint16_t documentBackground = _foregroundLoading ? TFT_WHITE : _document.backgroundColor();
  tft.fillRect(0, HeaderHeight, _display.width(),
               _display.height() - HeaderHeight - ControlsHeight, documentBackground);
  const int16_t textTop = HeaderHeight + DocumentPadding;
  if (_foregroundLoading) {
    drawLoadingPanel(true, true);
    return;
  }
  if (_message.length() > 0) {
    const String hint = shortText(_message, _display.width() - 28);
    const int16_t hintWidth = UiText::width(tft, hint, UiText::Size::Compact) + 14;
    const int16_t hintX = (_display.width() - hintWidth) / 2;
    const uint16_t hintBackground = TFT_RED;
    const uint16_t hintForeground = TFT_WHITE;
    tft.fillRoundRect(hintX, textTop + 7, hintWidth, 26, 5, hintBackground);
    UiText::draw(tft, hint, hintX + 7, textTop + 12, hintForeground, hintBackground,
                 UiText::Size::Compact);
    return;
  }
  if (_document.rowCount() == 0) {
    UiText::draw(tft, "Page has no readable text", 10, textTop,
                 TFT_DARKGREY, documentBackground, UiText::Size::Compact);
    return;
  }
  const uint16_t start = _document.pageStart(_page);
  const uint16_t end = _document.pageEnd(_page);
  int16_t y = textTop;
  for (uint16_t index = start; index < end; index++) {
    const VisualRow *rowRecord = _document.row(index);
    if (!rowRecord) break;
    const VisualRow &row = *rowRecord;
    const int16_t height = _document.rowHeight(index, documentLayout());
    if (row.formControlIndex >= 0) {
      drawFormControl(index, y);
    } else if (row.image) {
      uint16_t color = row.imageLink ? LinkImageColor : ImagePlaceholderColor;
      const uint16_t rowBackground = row.style.hasBlockBackground ? row.style.blockBackgroundColor : documentBackground;
      int16_t width = imageWidth(row.imageLabel);
      const EmbeddedImage *image = row.imageIndex >= 0 ? _document.image(row.imageIndex) : nullptr;
      const bool hasThumbnail = image && image->pixels;
      if (hasThumbnail) {
        width = image->width;
      }
      if (row.style.hasBlockBackground) {
        tft.fillRect(DocumentSidePadding, y, _display.width() - DocumentSidePadding * 2, height,
                     row.style.blockBackgroundColor);
      } else if (row.style.hasBackground) {
        tft.fillRect(DocumentSidePadding, y, width, height, row.style.backgroundColor);
      }
      if (hasThumbnail) {
        drawThumbnail(row.imageIndex, DocumentSidePadding, y);
      } else {
        tft.drawXBitmap(DocumentSidePadding + 2, y + 2, ic_file, 16, 16, color);
        BrowserText::draw(tft, ImagePlaceholderLabel,
                          DocumentSidePadding + 22, y + 3, color, rowBackground);
      }
    } else {
      const uint16_t rowBackground = row.style.hasBlockBackground ? row.style.blockBackgroundColor : documentBackground;
      if (row.style.hasBlockBackground) {
        tft.fillRect(DocumentSidePadding, y, _display.width() - DocumentSidePadding * 2, height,
                     row.style.blockBackgroundColor);
      }
      for (uint8_t runIndex = 0; runIndex < row.runCount; runIndex++) {
        const VisualRun &run = row.runs[runIndex];
        const int16_t x = DocumentSidePadding + run.x;
        if (run.image) {
          const EmbeddedImage *image = run.imageIndex >= 0 ? _document.image(run.imageIndex) : nullptr;
          const bool hasDirectImageLink = image != nullptr;
          uint16_t color = (run.style.linkIndex || hasDirectImageLink) ? LinkImageColor : ImagePlaceholderColor;
          const int16_t imageHeight = image && image->pixels ? image->height : TextRowHeight;
          if (run.style.hasBackground) tft.fillRect(x, y, run.width, imageHeight, run.style.backgroundColor);
          const bool hasThumbnail = image && image->pixels;
          if (hasThumbnail) {
            drawThumbnail(run.imageIndex, x, y);
          } else {
            tft.drawXBitmap(x + 2, y + 3, ic_file, 16, 16, color);
            BrowserText::draw(tft, ImagePlaceholderLabel, x + 22, y + 3, color,
                              run.style.hasBackground ? run.style.backgroundColor : rowBackground, run.style.size);
          }
          continue;
        }
        if (run.style.hasBackground && run.width > 0) {
          tft.fillRect(x, y, run.width, height, run.style.backgroundColor);
        }
        const int16_t textY = y + (height - BrowserText::lineHeight(run.style.size)) / 2;
        BrowserText::draw(tft, run.text, x, textY, run.style.color,
                          run.style.hasBackground ? run.style.backgroundColor : rowBackground, run.style.size);
        if (run.style.underline && run.width > 0) {
          tft.drawFastHLine(x, y + height - 2, run.width, run.style.color);
        }
      }
    }
    y += height;
  }
}

void BrowserScreen::drawLoadingPanel(bool redrawPhase, bool drawBarFrame) {
  TFT_eSPI &tft = _display.tft();
  const int16_t documentHeight = _display.height() - HeaderHeight - ControlsHeight;
  const int16_t panelTop = HeaderHeight + (documentHeight - 74) / 2;
  const bool knownSize = _loadingTotalBytes > 0;
  uint8_t percent = 0;
  if (knownSize) {
    percent = (uint8_t)((uint64_t)_loadingBytes * 100 / (uint32_t)_loadingTotalBytes);
    if (percent > 100) percent = 100;
  }
  const String detail = knownSize
    ? String(percent) + F("%  ") + String(_loadingBytes / 1024) + F(" / ") +
      String(((uint32_t)_loadingTotalBytes + 1023) / 1024) + F(" KB")
    : (_loadingBytes ? String(_loadingBytes / 1024) + F(" KB received") : String(F("Please wait")));
  const int16_t barX = 18;
  const int16_t barY = panelTop + 54;
  const int16_t barWidth = _display.width() - barX * 2;

  if (redrawPhase) {
    tft.fillRect(0, panelTop, _display.width(), 27, TFT_WHITE);
    const String phase = shortText(_loadingPhase.length() ? _loadingPhase : String(F("Loading")),
                                   _display.width() - 28);
    UiText::draw(tft, phase, _display.width() / 2, panelTop + 18,
                 TFT_BLACK, TFT_WHITE, UiText::Size::Compact, MC_DATUM);
  }
  // Only this short line and the bar interior change during a transfer. This
  // leaves the loading title and the bar frame on screen, eliminating flicker.
  tft.fillRect(0, panelTop + 28, _display.width(), 21, TFT_WHITE);
  UiText::draw(tft, detail, _display.width() / 2, panelTop + 39,
               TFT_DARKGREY, TFT_WHITE, UiText::Size::Compact, MC_DATUM);
  if (knownSize && _lastLoadingKnownSize && !drawBarFrame) {
    UiKit::updateProgressBar(tft, barX, barY, barWidth, 12, _lastLoadingPercent, percent,
                             TFT_BLUE, TFT_WHITE);
  } else {
    UiKit::drawProgressBar(tft, barX, barY, barWidth, 12, percent, !knownSize,
                           TFT_BLUE, TFT_DARKGREY, TFT_WHITE, drawBarFrame,
                           millis() - _loadingStartedMs);
  }
  _lastLoadingPercent = percent;
  _lastLoadingKnownSize = knownSize;
}

void BrowserScreen::redrawDocument() {
  if (!_display.isReady()) return;
  BrowserDisplayLock lock;
  if (!lock.locked()) return;
  BrowserText::begin(_display.tft());
  drawDocument();
  BrowserText::end(_display.tft());
}

void BrowserScreen::drawControls() {
  TFT_eSPI &tft = _display.tft();
  if (_showStart) {
    const Rect buttons[] = {_startAddBounds, _startToggleBounds, _startPreviousBounds, _startNextBounds};
    const uint8_t count = _lifecycle.loading() ? 0 : startSiteCount();
    // Opening a URL and switching between the two lists do not inspect the
    // records being loaded on the other core, so keep those controls live.
    // Paging waits for the completed, consistent list hand-off.
    const bool active[] = {true, true, !_lifecycle.loading() && _startListScroll > 0,
                           !_lifecycle.loading() && _startListScroll + StartListVisibleRows < count};
    for (uint8_t i = 0; i < 4; i++) {
      const Rect &button = buttons[i];
      const uint16_t color = FooterButtonColor;
      const uint16_t iconColor = active[i] ? TFT_WHITE : 0x8410;
      tft.fillRect(button.x, button.y, button.w - 1, button.h, color);
      const int16_t centerX = button.x + button.w / 2;
      const int16_t centerY = button.y + button.h / 2;
      if (i == 0) UiKit::drawPlusIcon(tft, centerX, centerY, iconColor, color);
      else if (i == 1) {
        if (_showFavorites) UiKit::drawRecentIcon(tft, centerX, centerY, iconColor, color);
        else UiKit::drawFavoriteIcon(tft, centerX, centerY, iconColor, color);
      } else {
        UiKit::drawPageArrow(tft, centerX, centerY, true, i == 3, iconColor, color);
      }
    }
    tft.setTextDatum(TL_DATUM);
    return;
  }
  const Rect buttons[] = {_backBounds, _homeBounds, _previousBounds, _nextBounds};
  const bool loading = isLoading();
  for (uint8_t i = 0; i < 4; i++) {
    bool active = (i == 0 && (loading || !_showStart)) ||
      (i == 1 && !_showStart && !_foregroundLoading) ||
      (!_showStart && !_foregroundLoading && ((i == 2 && _page > 0) ||
      (i == 3 && hasNextPage())));
    const uint16_t color = FooterButtonColor;
    const Rect &button = buttons[i];
    tft.fillRect(button.x, button.y, button.w - 1, button.h, color);
    uint16_t iconColor = active ? TFT_WHITE : 0x8410;
    const int16_t centerX = button.x + button.w / 2;
    const int16_t centerY = button.y + button.h / 2;
    if (i == 0 && loading) UiKit::drawCloseIcon(tft, centerX, centerY, iconColor, color);
    else if (i == 0) UiKit::drawBackIcon(tft, centerX, centerY, iconColor, color);
    else if (i == 1) UiKit::drawHomeIcon(tft, centerX, centerY, iconColor, color);
    else UiKit::drawPageArrow(tft, centerX, centerY, true, i == 3, iconColor, color);
  }
  tft.setTextDatum(TL_DATUM);
}

void BrowserScreen::drawPageControls() {
  BrowserDisplayLock lock;
  if (!lock.locked()) return;
  TFT_eSPI &tft = _display.tft();
  const Rect buttons[] = {_previousBounds, _nextBounds};
  for (uint8_t index = 0; index < 2; index++) {
    const bool active = index == 0 ? _page > 0 : hasNextPage();
    const Rect &button = buttons[index];
    tft.fillRect(button.x, button.y, button.w - 1, button.h, FooterButtonColor);
    UiKit::drawPageArrow(tft, button.x + button.w / 2, button.y + button.h / 2,
                         true, index == 1, active ? TFT_WHITE : 0x8410,
                         FooterButtonColor);
  }
  tft.setTextDatum(TL_DATUM);
}

bool BrowserScreen::contains(const Rect &rect, int16_t x, int16_t y) const {
  return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
}

bool BrowserScreen::isLoading() const {
  if (_foregroundLoading) return true;
  if (_imageWorker.active()) return true;
  return _imageWorker.available() && _document.hasUnfinishedImages(_imageWorker.generation());
}

void BrowserScreen::beginLoading(const String &phase) {
  _foregroundLoading = true;
  _loadingBytes = 0;
  _loadingTotalBytes = -1;
  _loadingStartedMs = millis();
  _lastLoadingDrawMs = 0;
  _lastLoadingDrawBytes = 0;
  _lastLoadingPercent = 0;
  _lastLoadingKnownSize = false;
  _loadingPhase = phase;
  browserLog(String(F("progress: ")) + phase);
}

void BrowserScreen::setLoadingPhase(const String &phase, int32_t totalBytes) {
  if (!_foregroundLoading) return;
  const bool changed = _loadingPhase != phase || _loadingTotalBytes != totalBytes;
  _loadingPhase = phase;
  _loadingTotalBytes = totalBytes;
  if (changed) {
    browserLog(String(F("progress: ")) + phase +
               (totalBytes >= 0 ? String(F(", expected bytes=")) + String(totalBytes) : String()));
    refreshLoadingDisplay(true);
  }
}

void BrowserScreen::finishLoading() {
  if (!_foregroundLoading) return;
  browserLog(String(F("progress complete: ")) + _loadingPhase + F(", bytes=") + String(_loadingBytes));
  _foregroundLoading = false;
  _loadingPhase = "";
  _loadingTotalBytes = -1;
}

void BrowserScreen::refreshLoadingDisplay(bool force) {
  if (!_foregroundLoading || !_display.isReady()) return;
  const uint32_t now = millis();
  // Drawing each received byte would make slow responses slower. Refresh at
  // most eight times a second, while still updating immediately after a
  // phase change or every 4 KiB on fast transfers.
  if (!force && now - _lastLoadingDrawMs < 125 && _loadingBytes - _lastLoadingDrawBytes < 4096) return;
  _lastLoadingDrawMs = now;
  _lastLoadingDrawBytes = _loadingBytes;
  BrowserDisplayLock lock;
  if (!lock.locked()) return;
  drawLoadingPanel(force, false);
}

void BrowserScreen::cancelLoading() {
  browserLog(F("cancelling active transfer and image jobs"));
  releaseEmbeddedImages();
  if (_foregroundLoading && _pendingNavigation.active) {
    if (_pendingNavigation.submitted) {
      _pageLoader.cancel();
      setLoadingPhase(F("Cancelling"));
      draw();
      return;
    }
    _pendingNavigation = {};
    finishLoading();
    _message = "Loading cancelled";
    draw();
    return;
  }
  draw();
}

void BrowserScreen::startPendingPageLoad(uint32_t nowMs) {
  if (!_foregroundLoading || !_pendingNavigation.active ||
      _pendingNavigation.submitted) return;
  if (_lifecycle.loading()) {
    setLoadingPhase(F("Loading saved browser data"));
    return;
  }
  if (_imageWorker.active()) {
    if (nowMs - _pendingNavigation.waitStartedMs < NetworkLockTimeoutMs) return;
    browserLog(String(F("image cleanup wait timed out: waitedMs=")) +
               String(nowMs - _pendingNavigation.waitStartedMs) + F(", imageGeneration=") +
               String(_imageWorker.generation()));
    _pendingNavigation = {};
    finishLoading();
    _message = "Browser is busy; try again";
    draw();
    return;
  }
  setLoadingPhase(F("Preparing request"));
  if (!_pageLoader.load(_pendingNavigation.request, _pendingNavigation.options)) {
    browserLog(F("page loader rejected request"));
    _pendingNavigation = {};
    finishLoading();
    _message = "Page loader unavailable";
    draw();
    return;
  }
  _pendingNavigation.submitted = true;
}

void BrowserScreen::collectPageLoadProgress() {
  if (!_foregroundLoading || !_pendingNavigation.submitted) return;
  PageLoadProgress progress;
  if (!_pageLoader.takeProgress(progress)) return;
  const bool changed = progress.phase != _loadingPhase ||
                       progress.totalBytes != _loadingTotalBytes;
  _loadingBytes = progress.bytesReceived;
  setLoadingPhase(progress.phase, progress.totalBytes);
  if (!changed) refreshLoadingDisplay();
}

void BrowserScreen::collectPageLoadResult() {
  PageLoadResult result;
  if (!_pageLoader.takeResult(result)) return;
  if (!_pendingNavigation.active || !_pendingNavigation.submitted) {
    if (result.downloadPath.length()) _transport.discardDownload(result.downloadPath);
    browserLog(F("discarded page result without active navigation"));
    return;
  }
  handlePageLoadResult(result);
}

void BrowserScreen::abandonPageLoad() {
  if (!_pendingNavigation.active && !_foregroundLoading) return;
  _pageLoader.cancel();
  PageLoadResult ignored;
  if (_pageLoader.takeResult(ignored) && ignored.downloadPath.length()) {
    _transport.discardDownload(ignored.downloadPath);
  }
  _pendingNavigation = {};
  finishLoading();
}

String BrowserScreen::documentLinkAt(int16_t x, int16_t y) const {
  return _message.length() ? String() : _document.linkAt(_page, x, y, documentLayout());
}

int8_t BrowserScreen::documentFormControlAt(int16_t x, int16_t y) const {
  return _message.length() ? -1 : _document.formAt(_page, x, y, documentLayout());
}

void BrowserScreen::openFormTextInput(uint8_t index) {
  const FormControl *control = _document.formControl(index);
  if (!control) return;
  String title = "Edit " + _document.formControlLabel(index);
  if (_navigator.push(ScreenId::TextInput,
                      ScreenData::textInput(shortText(title, 120), false, control->value))) {
    _editingFormControl = index;
  }
}

void BrowserScreen::resetForm(uint8_t formIndex) {
  if (_document.resetForm(formIndex)) draw();
}

void BrowserScreen::handleFormControl(uint8_t index) {
  const FormControl *control = _document.formControl(index);
  if (!control || control->disabled || control->kind == FormControlKind::Unsupported) return;
  switch (control->kind) {
    case FormControlKind::Text:
    case FormControlKind::Textarea:
      openFormTextInput(index);
      return;
    case FormControlKind::Checkbox:
      if (_document.toggleFormControl(index)) draw();
      return;
    case FormControlKind::Radio:
      if (_document.toggleFormControl(index)) draw();
      return;
    case FormControlKind::Select:
      if (_document.toggleFormControl(index)) draw();
      return;
    case FormControlKind::Reset:
      if (control->formIndex >= 0) resetForm((uint8_t)control->formIndex);
      return;
    case FormControlKind::Submit:
      if (control->formIndex >= 0) submitForm((uint8_t)control->formIndex, index);
      else {
        _message = "Submit control is outside a form";
        draw();
      }
      return;
    default:
      return;
  }
}

void BrowserScreen::submitForm(uint8_t formIndex, int8_t submitControlIndex) {
  const FormData *form = _document.form(formIndex);
  if (!form) return;
  if (form->method == FormMethod::Unsupported) {
    _message = "This form method is unsupported";
    draw();
    return;
  }
  if (!form->urlEncoded) {
    _message = "This form encoding is unsupported";
    draw();
    return;
  }
  Url base;
  if (!parseUrl(_currentUrl, base)) {
    _message = "Form action could not be resolved";
    draw();
    return;
  }
  String target = resolveUrl(base, form->action);
  String payload;
  const uint16_t maxBytes = form->method == FormMethod::Post ? MaxFormPostBytes : 0;
  if (!_document.buildFormPayload(formIndex, submitControlIndex, payload, maxBytes)) {
    _message = "Form data is too large";
    draw();
    return;
  }
  if (form->method == FormMethod::Get) {
    if (payload.length()) target += target.indexOf('?') >= 0 ? '&' : '?';
    target += payload;
    browserLog(String(F("submitting GET form: ")) + target);
    openUrl(target, true, false);
    return;
  }
  browserLog(String(F("submitting POST form: target=")) + target + F(", bodyBytes=") + String(payload.length()));
  openRequest({HttpMethod::Post, target, payload}, true, false, false);
}

void BrowserScreen::openUrlInput() {
  _navigator.push(ScreenId::TextInput, ScreenData::textInput("Open URL", false));
}

bool BrowserScreen::openUrl(const String &value, bool addToHistory, bool addToRecent,
                            bool useSessionCache) {
  return openRequest({HttpMethod::Get, value, ""}, addToHistory, addToRecent, useSessionCache);
}

bool BrowserScreen::openRequest(const HttpRequest &initialRequest, bool addToHistory,
                                bool addToRecent, bool useSessionCache) {
  if (_foregroundLoading || _pendingNavigation.active) return false;
  _menuOpen = false;
  const bool hadDocument = !_showStart && _currentUrl.length() > 0;
  const String previousUrl = _currentUrl;
  String url;
  String error;
  if (!normalizeUrl(initialRequest.url, url, error)) {
    browserLog(String(F("URL rejected: ")) + error);
    _showStart = false;
    _message = error;
    releaseEmbeddedImages();
    clearRenderedDocumentStorage();
    draw();
    return false;
  }
  if (initialRequest.method == HttpMethod::Post && initialRequest.body.length() > MaxFormPostBytes) {
    _showStart = false;
    _message = "Form data is too large";
    releaseEmbeddedImages();
    clearRenderedDocumentStorage();
    draw();
    return false;
  }
  HttpRequest request = initialRequest;
  request.url = url;
  browserLog(String(F("navigation requested: method=")) +
             (request.method == HttpMethod::Post ? F("POST") : F("GET")) + F(", url=") + url +
             (request.method == HttpMethod::Post ? String(F(", bodyBytes=")) + String(request.body.length()) : String()));

  // Record the page we are leaving before its replacement starts loading.
  // A cancelled (or failed) request still leaves that page reachable through
  // Back; delaying this until after the transfer succeeds made the footer Back
  // button disappear after cancelling a navigation.
  const bool addedPreviousPageToHistory = addToHistory && hadDocument && previousUrl != url;
  if (addedPreviousPageToHistory) {
    browserLog(F("saving current page to back history"));
    addHistory(previousUrl);
  }

  // Invalidates queued image jobs before a new page takes the shared HTTP
  // client. A task already in a network read is allowed to finish safely;
  // its result is discarded.
  releaseEmbeddedImages();
  // The visual rows, links, and CSS rules contain Strings from the previous
  // document. Clearing their allocations before a new TLS handshake avoids
  // carrying an entire old page in scarce internal RAM while mbedTLS creates
  // its bignum working buffers.
  clearRenderedDocumentStorage();
  browserLog(String(F("previous document released: freeHeap=")) + String(ESP.getFreeHeap()) +
             F(", largestHeapBlock=") + String(ESP.getMaxAllocHeap()));

  _currentUrl = url;
  _showStart = false;
  _message = "";
  _title = "";
  _page = 0;
  _pendingNavigation = {};
  _pendingNavigation.active = true;
  _pendingNavigation.hadDocument = hadDocument;
  _pendingNavigation.addedPreviousPageToHistory = addedPreviousPageToHistory;
  _pendingNavigation.addToRecent = addToRecent;
  _pendingNavigation.waitStartedMs = millis();
  _pendingNavigation.previousUrl = previousUrl;
  _pendingNavigation.request = request;
  _pendingNavigation.options = {
    _androidUserAgent ? String(MobileUserAgent) : String(EspUserAgent),
    useSessionCache,
    MaxBodyBytes,
    MaxDownloadBytes
  };
  beginLoading(_imageWorker.active() ? F("Waiting for image cleanup") : F("Preparing request"));
  draw();
  return true;
}

void BrowserScreen::handlePageLoadResult(PageLoadResult &result) {
  const PendingNavigation navigation = _pendingNavigation;
  _pendingNavigation = {};

  if (result.outcome == Browser::PageLoadOutcome::Error) {
    browserLog(String(F("navigation failed: url=")) + navigation.request.url +
               F(", phase=") + _loadingPhase + F(", error=") + result.error +
               F(", userCancelled=") + (result.cancelled ? F("yes") : F("no")));
    if (result.error == F("Connect to Wi-Fi first")) {
      _currentUrl = navigation.previousUrl;
      if (navigation.addedPreviousPageToHistory && _historyCount > 0 &&
          _historyUrls[_historyCount - 1] == navigation.previousUrl) --_historyCount;
    }
    if (result.errorKind == Browser::PageLoadErrorKind::TextEncoding) {
      _currentUrl = result.finalUrl.length() ? result.finalUrl : navigation.request.url;
      finishLoading();
      renderPlainText("(" + result.error + ")", "Unsupported text encoding");
      _message = "";
    } else if (result.errorKind == Browser::PageLoadErrorKind::UnsupportedContent) {
      _currentUrl = result.finalUrl.length() ? result.finalUrl : navigation.request.url;
      finishLoading();
      renderPlainText("[" + result.error + "]", "Unsupported content");
      if (navigation.popHistoryOnSuccess && _historyCount > 0) --_historyCount;
      if (navigation.addToRecent) addRecent(_currentUrl, _title);
      saveState();
      _message = "";
    } else {
      finishLoading();
      _message = result.cancelled ? String("Loading cancelled") : result.error;
    }
    draw();
    return;
  }

  _currentUrl = result.finalUrl.length() ? result.finalUrl : navigation.request.url;
  if (result.outcome == Browser::PageLoadOutcome::Download) {
    setLoadingPhase(F("Opening downloaded file"));
    const FileFormats::Format format = FileFormats::fromPath(result.downloadPath);
    const FileFormats::Group group = FileFormats::group(format);
    if (navigation.addedPreviousPageToHistory && _historyCount > 0 &&
        _historyUrls[_historyCount - 1] == navigation.previousUrl) {
      --_historyCount;
    }
    if (navigation.popHistoryOnSuccess && _historyCount > 0) --_historyCount;
    _downloadReturnUrl = navigation.hadDocument ? navigation.previousUrl : String();
    if (navigation.addToRecent) addRecent(_currentUrl, _title);
    saveState();
    finishLoading();
    browserLog(String(F("download complete: ")) + result.downloadPath);
    if (group == FileFormats::Group::Image &&
        _navigator.push(ScreenId::ImageViewer, ScreenData::imagePath(result.downloadPath))) return;
    if (group == FileFormats::Group::Audio &&
        _navigator.push(ScreenId::AudioPlayer, ScreenData::audioPath(result.downloadPath))) return;
    if (group == FileFormats::Group::Video &&
        _navigator.push(ScreenId::VideoPlayer, ScreenData::videoPath(result.downloadPath))) return;
    _downloadReturnUrl = "";
    _message = "Could not open downloaded file";
    draw();
    return;
  }

  setLoadingPhase(result.fromSessionCache ? F("Rendering cached page") : F("Rendering page"));
  const bool htmlPage = result.contentType.indexOf("text/html") >= 0 ||
                        result.contentType.indexOf("application/xhtml") >= 0 ||
                        result.contentType.length() == 0;
  if (htmlPage) renderDocument(result.textBody);
  else renderPlainText(result.textBody, "Text page");
  if (navigation.popHistoryOnSuccess && _historyCount > 0) {
    --_historyCount;
    browserLog(F("back navigation complete"));
  }
  if (navigation.addToRecent) addRecent(_currentUrl, _title);
  saveState();
  finishLoading();
  browserLog(String(F("page ready: title=\"")) + _title + F("\", rows=") +
             String(_document.rowCount()) + F(", pages=") + String(_document.pageCount()) +
             F(", images=") + String(_document.imageCount()) +
             (result.fromSessionCache ? F(", session cache") : F("")));
  draw();
}

void BrowserScreen::goBack() {
  if (_historyCount == 0) {
    browserLog(F("back history empty; showing start page"));
    releaseEmbeddedImages();
    _showStart = true;
    _page = 0;
    _message = "";
    draw();
    return;
  }
  const String target = _historyUrls[_historyCount - 1];
  browserLog(String(F("loading previous page: ")) + target);
  if (openUrl(target, false, false, true)) {
    _pendingNavigation.popHistoryOnSuccess = true;
  }
}

bool BrowserScreen::normalizeUrl(const String &value, String &normalized, String &error) const {
  return BrowserTransport::normalizeUrl(value, normalized, error);
}

bool BrowserScreen::parseUrl(const String &value, Url &url) const {
  return BrowserTransport::parseUrl(value, url);
}

String BrowserScreen::resolveUrl(const Url &base, const String &reference) const {
  return BrowserTransport::resolveUrl(base, reference);
}

void BrowserScreen::renderDocument(const String &html) {
  releaseEmbeddedImages();
  _editingFormControl = -1;
  _document.parseHtml(html, {_imageWorker.generation(), this, resolveDocumentUrl}, documentLayout());
  _title = _document.title();
  browserLog(String(F("document rendered: html bytes=")) + String(html.length()) +
             F(", links=") + String(_document.linkCount()) +
             F(", image placeholders=") + String(_document.imageCount()));
}
void BrowserScreen::renderPlainText(const String &text, const String &title) {
  releaseEmbeddedImages();
  _document.parsePlainText(text, title, documentLayout(), DefaultTextColor, TFT_WHITE);
  _title = _document.title();
  _editingFormControl = -1;
  browserLog(String(F("plain text rendered: bytes=")) + String(text.length()) +
             F(", rows=") + String(_document.rowCount()));
}

void BrowserScreen::clearRenderedDocumentStorage() {
  _document.clear();
  _editingFormControl = -1;
}

void BrowserScreen::drawThumbnail(int16_t imageIndex, int16_t x, int16_t y) const {
  const EmbeddedImage *image = imageIndex >= 0 ? _document.image(imageIndex) : nullptr;
  if (!image || !image->pixels || !image->width || !image->height) return;
  _display.tft().pushImage(x, y, image->width, image->height, image->pixels);
}

void BrowserScreen::drawFormControl(uint16_t rowIndex, int16_t y) {
  const VisualRow *rowRecord = _document.row(rowIndex);
  if (!rowRecord || rowRecord->formControlIndex < 0) return;
  const VisualRow &row = *rowRecord;
  const uint8_t controlIndex = static_cast<uint8_t>(row.formControlIndex);
  const FormControl *controlRecord = _document.formControl(controlIndex);
  if (!controlRecord) return;
  TFT_eSPI &tft = _display.tft();
  const FormControl &control = *controlRecord;
  const int16_t x = DocumentSidePadding;
  const int16_t width = _display.width() - DocumentSidePadding * 2;
  const int16_t height = _document.rowHeight(rowIndex, documentLayout());
  const uint16_t border = control.disabled ? TFT_DARKGREY : TFT_DARKCYAN;
  const uint16_t fill = control.disabled ? TFT_LIGHTGREY : TFT_WHITE;
  const uint16_t textColor = TFT_DARKGREY;
  const uint16_t rowBackground = row.style.hasBlockBackground ? row.style.blockBackgroundColor
    : (row.style.hasBackground ? row.style.backgroundColor : _document.backgroundColor());
  const UiText::Size labelSize = control.labelIsPlaceholder
    ? UiText::Size::Compact : row.style.size;
  const String label = UiText::ellipsize(_document.formControlLabel(controlIndex), width - 14, labelSize);

  if (control.kind == FormControlKind::Submit || control.kind == FormControlKind::Reset) {
    const uint16_t buttonColor = control.disabled ? TFT_DARKGREY :
      (control.kind == FormControlKind::Submit ? TFT_BLUE : TFT_DARKCYAN);
    tft.fillSmoothRoundRect(x, y + 4, width, height - 8, 5, buttonColor, rowBackground);
    const String centered = UiText::ellipsize(label, width - 14, UiText::Size::Content);
    UiText::draw(tft, centered, x + width / 2, y + height / 2,
                 TFT_WHITE, buttonColor, UiText::Size::Content, MC_DATUM);
    return;
  }
  if (control.kind == FormControlKind::Checkbox || control.kind == FormControlKind::Radio) {
    const int16_t markerX = x + 7;
    const int16_t markerY = y + (height - 18) / 2;
    if (control.kind == FormControlKind::Checkbox) {
      tft.drawRoundRect(markerX, markerY, 18, 18, 3, border);
      if (control.checked) {
        tft.fillRoundRect(markerX + 3, markerY + 3, 12, 12, 2, TFT_BLUE);
        tft.drawLine(markerX + 5, markerY + 9, markerX + 8, markerY + 13, TFT_WHITE);
        tft.drawLine(markerX + 8, markerY + 13, markerX + 14, markerY + 5, TFT_WHITE);
      }
    } else {
      tft.drawCircle(markerX + 9, markerY + 9, 8, border);
      if (control.checked) tft.fillCircle(markerX + 9, markerY + 9, 4, TFT_BLUE);
    }
    UiText::draw(tft, UiText::ellipsize(label, width - 34, labelSize), markerX + 25,
                 y + (height - UiText::lineHeight(labelSize)) / 2, textColor,
                 _document.backgroundColor(), labelSize);
    return;
  }
  tft.fillRoundRect(x, y + 2, width, height - 4, 4, fill);
  tft.drawRoundRect(x, y + 2, width, height - 4, 4, border);
  UiText::draw(tft, label, x + 7, y + 6, textColor, fill, labelSize);
  String value = _document.formControlValue(controlIndex);
  if (control.kind == FormControlKind::Unsupported) value = "Unsupported";
  const UiText::Size valueSize = row.style.size;
  value = UiText::ellipsize(value, width - 30, valueSize);
  UiText::draw(tft, value, x + 7, y + 23, control.disabled ? TFT_DARKGREY : TFT_BLACK, fill, valueSize);
  if (control.kind == FormControlKind::Select) {
    tft.fillTriangle(x + width - 18, y + height / 2 - 3, x + width - 8, y + height / 2 - 3,
                     x + width - 13, y + height / 2 + 4, control.disabled ? TFT_DARKGREY : TFT_DARKCYAN);
  }
}

bool BrowserScreen::resolveDocumentUrl(void *context, const String &reference, String &resolved) {
  BrowserScreen *screen = static_cast<BrowserScreen *>(context);
  if (!screen) return false;
  Url base;
  String error;
  if (!screen->parseUrl(screen->_currentUrl, base)) return false;
  const String candidate = screen->resolveUrl(base, reference);
  return screen->normalizeUrl(candidate, resolved, error);
}

void BrowserScreen::releaseEmbeddedImages() {
  const uint16_t imageCount = _document.imageCount();
  if (imageCount) browserLog(String(F("releasing image jobs and previews: count=")) + String(imageCount));
  _imageWorker.cancel();
  _document.clearImageDescriptors();
  _nextImageToQueue = 0;
}

void BrowserScreen::pumpImageJobs() {
  if (_showStart || _message.length()) return;
  const uint32_t generation = _imageWorker.generation();
  const String userAgent = _androidUserAgent ? String(MobileUserAgent) : String(EspUserAgent);
  while (_nextImageToQueue < _document.imageCount()) {
    const uint16_t index = _nextImageToQueue++;
    ImageJob job{};
    // The image cache belongs to the browser session, not one cached page.
    // A shared icon on the next site can therefore decode locally too.
    if (!_document.beginImageJob(index, generation, _sessionCache.active(), job)) continue;
    if (!_imageWorker.available()) {
      _document.markImageFailed(index, generation);
      browserLog(String(F("image preview unavailable #")) + String(index + 1));
      continue;
    }
    if (!_imageWorker.enqueue(job, userAgent)) {
      _document.returnImageJobToPending(index, generation);
      _nextImageToQueue--;
      return;
    }
    browserLog(String(F("image job queued #")) + String(index + 1));
  }
}

void BrowserScreen::collectImageResults() {
  bool redraw = false;
  bool redrawCurrentPage = false;
  const bool wasLoading = isLoading();
  ImageResult result{};
  while (_imageWorker.take(result)) {
    if (result.generation == _imageWorker.generation() && result.pixels && result.width && result.height &&
        _document.setImageResult(result.imageIndex, result.generation, result.pixels,
                                 result.width, result.height)) {
      if (imageIsVisibleOnCurrentPage(result.imageIndex)) redrawCurrentPage = true;
      browserLog(String(F("image preview ready #")) + String(result.imageIndex + 1) + F(" (") +
                 String(result.width) + F("x") + String(result.height) + F(")"));
      redraw = true;
      continue;
    }
    if (result.generation == _imageWorker.generation() &&
        _document.markImageFailed(result.imageIndex, result.generation)) {
      browserLog(String(F("image preview failed #")) + String(result.imageIndex + 1));
    }
    _imageWorker.discard(result);
  }
  if (redraw && !_showStart && _message.length() == 0) {
    reflowDocument();
    // Do not repaint a page that has no visible placeholder to replace. It
    // avoids a perceptible flash while images below or above this page load.
    if (redrawCurrentPage) redrawDocument();
  }
  if (wasLoading != isLoading() && !_showStart) {
    BrowserDisplayLock lock;
    if (lock.locked()) drawControls();
  }
}

bool BrowserScreen::imageIsVisibleOnCurrentPage(uint16_t imageIndex) const {
  return _document.imageVisibleOnPage(_page, imageIndex);
}

void BrowserScreen::reflowDocument() {
  _document.reflow(documentLayout());
  if (_page >= _document.pageCount()) _page = 0;
}

bool BrowserScreen::hasNextPage() const {
  return _page + 1 < _document.pageCount();
}

String BrowserScreen::shortText(const String &value, int16_t maxWidth) const {
  return BrowserText::ellipsize(_display.tft(), value, maxWidth);
}

String BrowserScreen::lower(const String &value) const {
  String result = value;
  result.toLowerCase();
  return result;
}

int16_t BrowserScreen::imageWidth(const String &label) const {
  (void)label;
  return 26 + BrowserText::measure(ImagePlaceholderLabel);
}

void BrowserScreen::addHistory(const String &url) {
  if (url.length() == 0) return;
  if (_historyCount >= MaxHistory) {
    for (uint8_t index = 1; index < MaxHistory; index++) {
      _historyUrls[index - 1] = _historyUrls[index];
    }
    _historyCount = MaxHistory - 1;
  }
  _historyUrls[_historyCount++] = url;
  browserLog(String(F("history stored: entries=")) + String(_historyCount));
}

void BrowserScreen::addRecent(const String &url, const String &title) {
  _persistentState.addRecent(url, title);
  browserLog(String(F("recent site saved: entries=")) + String(_persistentState.recentCount()));
}

uint8_t BrowserScreen::startSiteCount() const {
  return _showFavorites ? _persistentState.favoriteCount() : _persistentState.recentCount();
}

const BrowserScreen::RecentSite &BrowserScreen::startSiteAt(uint8_t index) const {
  return _showFavorites ? _persistentState.favoriteAt(index) : _persistentState.recentAt(index);
}

void BrowserScreen::clampStartListScroll() {
  const uint8_t count = startSiteCount();
  if (count == 0) {
    _startListScroll = 0;
    return;
  }
  const uint8_t finalPage = ((count - 1) / StartListVisibleRows) * StartListVisibleRows;
  if (_startListScroll > finalPage) _startListScroll = finalPage;
  else _startListScroll = (_startListScroll / StartListVisibleRows) * StartListVisibleRows;
}

bool BrowserScreen::isFavorite(const String &url) const {
  return url.length() && _persistentState.isFavorite(url);
}

void BrowserScreen::addFavorite(const String &url, const String &title) {
  if (url.length() == 0 || isFavorite(url)) return;
  if (_persistentState.favoriteCount() >= MaxRecentSites) {
    browserLog(F("favorite site ignored: list is full"));
    return;
  }
  _persistentState.addFavorite(url, title);
  browserLog(String(F("favorite site saved: entries=")) + String(_persistentState.favoriteCount()));
}

void BrowserScreen::removeRecent(uint8_t index) {
  if (index >= _persistentState.recentCount()) return;
  _persistentState.removeRecent(index);
  clampStartListScroll();
  browserLog(String(F("recent site removed: entries=")) + String(_persistentState.recentCount()));
}

void BrowserScreen::removeFavorite(const String &url) {
  if (!isFavorite(url)) return;
  _persistentState.removeFavorite(url);
  clampStartListScroll();
  browserLog(String(F("favorite site removed: entries=")) + String(_persistentState.favoriteCount()));
}

void BrowserScreen::toggleFavorite() {
  if (_currentUrl.length() == 0) return;
  if (isFavorite(_currentUrl)) removeFavorite(_currentUrl);
  else addFavorite(_currentUrl, _title);
  saveState();
  _menuOpen = false;
  draw();
}

void BrowserScreen::toggleUserAgent() {
  _androidUserAgent = !_androidUserAgent;
  _menuOpen = false;
  browserLog(_androidUserAgent ? F("Android user agent enabled") : F("ESP user agent enabled"));
  // Re-fetch the same URL without touching Browser history or the persistent
  // recent list, so the changed identity is reflected immediately.
  openUrl(_currentUrl, false, false, false);
}

void BrowserScreen::loadState() {
  _persistentState.load();
}

void BrowserScreen::saveState() {
  _persistentState.save();
}
