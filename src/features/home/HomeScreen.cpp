#include "HomeScreen.h"

#include <ctype.h>
#include <limits.h>
#include <string.h>

#include "../../app/Navigator.h"
#include "../../features/apps/AppPackage.h"
#include "../../ui/UiKit.h"
#include "../../ui/UiText.h"
#include "../../ui/icons.h"

namespace {
constexpr uint16_t StatusActiveColor = TFT_WHITE;
constexpr uint16_t StatusInactiveColor = TFT_DARKGREY;

int compareFolded(const char *left, const char *right) {
  const unsigned char *a = reinterpret_cast<const unsigned char *>(left);
  const unsigned char *b = reinterpret_cast<const unsigned char *>(right);
  while (*a && *b) {
    const unsigned char foldedA = *a < 0x80 ? static_cast<unsigned char>(tolower(*a)) : *a;
    const unsigned char foldedB = *b < 0x80 ? static_cast<unsigned char>(tolower(*b)) : *b;
    if (foldedA != foldedB) return foldedA < foldedB ? -1 : 1;
    ++a;
    ++b;
  }
  return *a == *b ? 0 : (*a ? 1 : -1);
}

void hashBytes(uint64_t &hash, const void *data, size_t length) {
  const uint8_t *bytes = static_cast<const uint8_t *>(data);
  for (size_t index = 0; index < length; ++index) {
    hash ^= bytes[index];
    hash *= 1099511628211ULL;
  }
}
}

HomeScreen::HomeScreen(Navigator &navigator,
                       TouchController &touch,
                       DisplayController &display,
                       SdCardController &sdCard,
                       WiFiController &wifi,
                       TimeController &time,
                       BatteryHistoryService &batteryHistory)
  : Screen(navigator, touch, display),
    _sdCard(sdCard),
    _wifi(wifi),
    _time(time),
    _batteryHistory(batteryHistory),
    _catalogLoader(sdCard),
    _pageStrip(&display.tft()),
    _pageFrame(&display.tft()),
    _catalogRevision(0),
    _sdMounted(false),
    _wifiConnected(false),
    _batteryVoltage(0.0f),
    _gestureActive(false),
    _gestureCancelled(false),
    _horizontalDrag(false),
    _filesHoldTriggered(false),
    _settling(false),
    _pageStripReady(false),
    _pressedIndex(-1),
    _gestureStartX(0),
    _gestureStartY(0),
    _gestureStartedMs(0),
    _velocityStartX(0),
    _velocityEndX(0),
    _velocityStartMs(0),
    _velocityEndMs(0),
    _gestureBaseScroll(0),
    _visibleScroll(0),
    _renderedScroll(INT32_MIN),
    _settleFromScroll(0),
    _settleToScroll(0),
    _settleStartedMs(0),
    _settleDurationMs(0),
    _lastAnimationFrameMs(0),
    _lastClockCheckMs(0xffffffffUL),
    _lastBatteryGeneration(0),
    _lastClockText(""),
    _appCount(0),
    _page(0) {
  rebuildApps();
}

void HomeScreen::onEnter() {
  refreshSdState();
  _wifiConnected = _wifi.isConnected();
  _batteryVoltage = _batteryHistory.latest().smoothVoltage;
  _lastBatteryGeneration = _batteryHistory.generation();
  _lastClockCheckMs = 0xffffffffUL;
  _lastClockText = "";
  resetGesture();
  rebuildApps();
  draw();
  if (_sdMounted) _catalogLoader.requestScan();
}

void HomeScreen::onResume() {
  const bool wasMounted = _sdMounted;
  refreshSdState();
  _wifiConnected = _wifi.isConnected();
  _batteryVoltage = _batteryHistory.latest().smoothVoltage;
  _lastBatteryGeneration = _batteryHistory.generation();
  _lastClockCheckMs = 0xffffffffUL;
  _lastClockText = "";
  cancelMotion();

  const bool hasCachedPage = _pageStripReady && _pageFrame.created();
  if (hasCachedPage) restoreCachedPage();
  if (wasMounted && !_sdMounted) {
    _catalogLoader.cancel();
    AppCatalogLoader::clearCatalog(_pendingCatalog);
    updateCatalogTiles();
  }
  if (!hasCachedPage) draw();
  if (_sdMounted) _catalogLoader.requestScan();
}

void HomeScreen::onExit() {
  cancelMotion();
}

bool HomeScreen::releaseResources() {
  destroyPageStrip();
  _catalogLoader.stop();
  if (!_catalogLoader.releaseResources()) return false;
  AppCatalogLoader::clearCatalog(_catalog);
  AppCatalogLoader::clearCatalog(_pendingCatalog);
  return true;
}

void HomeScreen::handleTouch(const TouchPoint &point) {
  const uint32_t nowMs = millis();
  if (!point.touched) {
    if (_gestureActive) finishGesture(nowMs);
    return;
  }

  if (!_gestureActive) {
    if (_settling) {
      _visibleScroll = _settleToScroll;
      _page = static_cast<uint8_t>(_settleToScroll / _display.width());
      _settling = false;
      renderPageViewport(true);
    }
    _gestureActive = true;
    _gestureCancelled = false;
    _horizontalDrag = false;
    _filesHoldTriggered = false;
    _gestureStartX = point.x;
    _gestureStartY = point.y;
    _gestureStartedMs = nowMs;
    _gestureBaseScroll = _visibleScroll;
    _velocityStartX = _velocityEndX = static_cast<int16_t>(point.x);
    _velocityStartMs = _velocityEndMs = nowMs;
    _pressedIndex = appAt(point.x, point.y);
    return;
  }

  updateGesture(point.x, point.y);
}

void HomeScreen::onResult(const ScreenResult &result) {
  if (result.kind != ScreenResultKind::Text || result.text.length() != 4) return;
  for (uint8_t i = 0; i < 4; i++) if (result.text[i] < '0' || result.text[i] > '9') return;
  const uint16_t pin = static_cast<uint16_t>(result.text.toInt());
  _navigator.push(ScreenId::Files, ScreenData::secretFiles(pin));
}

void HomeScreen::tick(uint32_t nowMs) {
  const TouchPoint held = _touch.point();
  if (_gestureActive && held.touched) updateGesture(held.x, held.y);
  if (_settling) updateSettle(nowMs);

  if (_gestureActive && !_gestureCancelled && !_filesHoldTriggered &&
      _pressedIndex >= 0 && _pressedIndex < _appCount &&
      _apps[_pressedIndex].action == AppAction::Files) {
    const AppTile &files = _apps[_pressedIndex];
    const bool inside = held.touched && held.x >= files.x && held.x < files.x + files.w &&
                        held.y >= files.y && held.y < files.y + files.h;
    if (inside && nowMs - _gestureStartedMs >= 1000) {
      _filesHoldTriggered = true;
      _gestureCancelled = true;
      _navigator.push(ScreenId::TextInput, ScreenData::pinInput("Secret PIN"));
      return;
    }
  }

  if (_lastClockCheckMs == 0xffffffffUL || nowMs - _lastClockCheckMs >= 1000UL) {
    drawClock(false);
    _lastClockCheckMs = nowMs;
  }

  const uint32_t batteryGeneration = _batteryHistory.generation();
  if (batteryGeneration != _lastBatteryGeneration) {
    setBatteryVoltage(_batteryHistory.latest().smoothVoltage);
    _lastBatteryGeneration = batteryGeneration;
  }

  handleSdCardEvent(_sdCard.takeEvent());
  setWifiState(_wifi.isConnected());

  if (!_gestureActive && !_settling &&
      _catalogLoader.takeCatalog(_pendingCatalog, _catalogRevision)) {
    updateCatalogTiles();
  }
}

void HomeScreen::draw() {
  if (!_display.isReady()) return;
  layoutApps();
  cancelMotion();
  _visibleScroll = static_cast<int32_t>(_page) * _display.width();
  _renderedScroll = INT32_MIN;
  _display.tft().fillScreen(TFT_BLACK);
  drawStatusBar();
  if (buildPageStrip()) renderPageViewport(true);
  else {
    drawApps();
    drawPageIndicator();
  }
}

void HomeScreen::rebuildApps() {
  _appCount = 0;
  auto addBuiltIn = [&](const char *label, AppAction action, uint16_t color) {
    AppTile &app = _apps[_appCount++];
    memset(&app, 0, sizeof(app));
    strncpy(app.label, label, sizeof(app.label) - 1);
    app.action = action;
    app.circleColor = color;
  };
  addBuiltIn("AI Assistant", AppAction::AiAssistant, 0xfe67);
  addBuiltIn("Browser", AppAction::Browser, 0xfb17);
  addBuiltIn("Files", AppAction::Files, 0x065f);
  addBuiltIn("Recorder", AppAction::Recorder, 0x9adf);
  addBuiltIn("Settings", AppAction::Settings, 0xfda0);
  addBuiltIn("USB Drive", AppAction::UsbDrive, 0x0799);
  addBuiltIn("Workout", AppAction::Workout, 0x04df);
  addBuiltIn("Mac Remote", AppAction::MacRemote, 0x051d);

  for (uint8_t index = 0; index < _catalog.count && _appCount < MaxHomeApps; ++index) {
    const AppCatalogLoader::Entry &source = _catalog.entries[index];
    AppTile &app = _apps[_appCount++];
    memset(&app, 0, sizeof(app));
    strncpy(app.label, source.name, sizeof(app.label) - 1);
    strncpy(app.externalId, source.id, sizeof(app.externalId) - 1);
    app.action = AppAction::External;
    app.circleColor = source.iconBackground;
    app.iconColors = source.iconColors;
    app.iconAlpha = source.iconAlpha;
  }
  sortApps();
  const uint8_t pages = pageCount();
  if (_page >= pages) _page = pages - 1;
  layoutApps();
}

void HomeScreen::sortApps() {
  for (uint8_t index = 1; index < _appCount; ++index) {
    AppTile value = _apps[index];
    uint8_t position = index;
    while (position > 0) {
      const AppTile &previous = _apps[position - 1];
      int comparison = compareFolded(previous.label, value.label);
      if (comparison == 0) comparison = strcmp(previous.label, value.label);
      if (comparison == 0 && previous.action != value.action) {
        comparison = previous.action == AppAction::External ? 1 : -1;
      }
      if (comparison == 0) comparison = strcmp(previous.externalId, value.externalId);
      if (comparison <= 0) break;
      _apps[position] = _apps[position - 1];
      --position;
    }
    _apps[position] = value;
  }
}

void HomeScreen::layoutApps() {
  const int16_t width = _display.width();
  const int16_t columnWidth = width / AppColumns;
  const int16_t tileTop = StatusBarHeight + 16;
  const int16_t tileHeight = 116;
  for (uint8_t index = 0; index < _appCount; ++index) {
    const uint8_t slot = index % PageSize;
    const uint8_t column = slot % AppColumns;
    const uint8_t row = slot / AppColumns;
    _apps[index].x = column * columnWidth;
    _apps[index].y = tileTop + row * tileHeight;
    _apps[index].w = column == AppColumns - 1 ? width - _apps[index].x : columnWidth;
    _apps[index].h = tileHeight;
  }
}

void HomeScreen::drawStatusBar() {
  TFT_eSPI &tft = _display.tft();
  tft.fillRect(0, 0, _display.width(), StatusBarHeight, TFT_BLACK);
  drawClock(true);
  drawSdIcon();
  drawWifiIcon();
  drawBattery();
}

void HomeScreen::drawClock(bool force) {
  if (!_display.isReady()) return;
  struct tm localTimeValue = {};
  String text = _time.localTime(localTimeValue) ? clockText(localTimeValue) : "--:--";
  if (!force && text == _lastClockText) return;
  TFT_eSPI &tft = _display.tft();
  tft.fillRect(0, 0, 72, StatusBarHeight, TFT_BLACK);
  UiText::draw(tft, text, 8, StatusBarHeight / 2, TFT_WHITE, TFT_BLACK,
               UiText::Size::Compact, ML_DATUM);
  _lastClockText = text;
}

void HomeScreen::drawSdIcon() {
  TFT_eSPI &tft = _display.tft();
  const int16_t x = _display.width() - 90;
  const int16_t y = 4;
  const uint16_t color = _sdMounted ? StatusActiveColor : StatusInactiveColor;
  tft.fillRect(x, y, ic_sd_card_width, ic_sd_card_height, TFT_BLACK);
  UiKit::drawAlphaIcon(tft, x, y, ic_sd_card_width, ic_sd_card_height,
                       ic_sd_card_alpha, color, TFT_BLACK);
}

void HomeScreen::drawWifiIcon() {
  TFT_eSPI &tft = _display.tft();
  const int16_t x = _display.width() - 66;
  const int16_t y = 4;
  const uint16_t color = _wifiConnected ? StatusActiveColor : StatusInactiveColor;
  tft.fillRect(x, y, ic_wifi_width, ic_wifi_height, TFT_BLACK);
  UiKit::drawAlphaIcon(tft, x, y, ic_wifi_width, ic_wifi_height,
                       ic_wifi_alpha, color, TFT_BLACK);
}

void HomeScreen::drawBattery() {
  UiKit::drawBatteryVoltage(_display.tft(), _display.width() - 46, 4,
                            _batteryVoltage, TFT_BLACK);
}

void HomeScreen::drawApps() {
  TFT_eSPI &tft = _display.tft();
  const uint8_t first = _page * PageSize;
  const uint8_t end = min<uint8_t>(_appCount, first + PageSize);
  for (uint8_t index = first; index < end; ++index) {
    drawAppTileTo(tft, _apps[index], 0, 0);
  }
}

template <typename Canvas>
void HomeScreen::drawAppTileTo(Canvas &canvas, const AppTile &app,
                               int16_t xOffset, int16_t yOffset) {
  const int16_t centerX = xOffset + app.x + app.w / 2;
  const int16_t centerY = yOffset + app.y + 48;
  canvas.fillSmoothCircle(centerX, centerY, IconDiameter / 2,
                          app.circleColor, TFT_BLACK);
  drawAppIconTo(canvas, app, centerX, centerY);
  const String label = truncateLabel(String(app.label), app.w - 12);
  UiText::draw(canvas, label, centerX, yOffset + app.y + 94,
               TFT_WHITE, TFT_BLACK, UiText::Size::Compact, TC_DATUM);
}

void HomeScreen::drawPageIndicator() {
  drawPageIndicatorTo(_display.tft(), 0);
}

void HomeScreen::drawPageIndicatorTo(TFT_eSPI &target, int16_t yOffset) {
  constexpr int16_t indicatorTop = 436;
  constexpr int16_t indicatorHeight = 25;
  target.fillRect(0, indicatorTop - yOffset,
                  _display.width(), indicatorHeight, TFT_BLACK);
  const uint8_t pages = pageCount();
  if (pages <= 1) return;
  constexpr int16_t spacing = 14;
  const int16_t startX = (_display.width() - (pages - 1) * spacing) / 2;
  constexpr int16_t y = 448;
  for (uint8_t index = 0; index < pages; ++index) {
    target.fillSmoothCircle(startX + index * spacing, y - yOffset, 3,
                            TFT_DARKGREY, TFT_BLACK);
  }
  const int32_t maximum = static_cast<int32_t>(pages - 1) * _display.width();
  const int32_t clamped = min<int32_t>(maximum, max<int32_t>(0, _visibleScroll));
  const int16_t markerX = startX + static_cast<int16_t>(
      (clamped * spacing + _display.width() / 2) / _display.width());
  target.fillSmoothCircle(markerX, y - yOffset, 4, TFT_WHITE, TFT_BLACK);
}

template <typename Canvas>
void HomeScreen::drawAppIconTo(Canvas &canvas, const AppTile &app,
                               int16_t centerX, int16_t centerY) {
  if (app.action == AppAction::External) {
    if (app.iconColors && app.iconAlpha) {
      UiKit::drawRgba565IconFromMemory(
          canvas, centerX - AppCatalogLoader::IconWidth / 2,
          centerY - AppCatalogLoader::IconHeight / 2,
          AppCatalogLoader::IconWidth, AppCatalogLoader::IconHeight,
          app.iconColors, app.iconAlpha, app.circleColor);
    } else {
      UiKit::drawAppsIcon(canvas, centerX, centerY, TFT_WHITE, app.circleColor);
    }
    return;
  }
  if (app.action == AppAction::MacRemote) {
    UiKit::drawMacRemoteIcon(canvas, centerX, centerY, TFT_WHITE,
                             app.circleColor);
    return;
  }
  const uint16_t *colors = nullptr;
  const uint8_t *alpha = nullptr;
  uint16_t width = 0;
  uint16_t height = 0;
  if (app.action == AppAction::AiAssistant) {
    colors = ic_ai_colors; alpha = ic_ai_alpha; width = ic_ai_width; height = ic_ai_height;
  } else if (app.action == AppAction::Recorder) {
    colors = ic_recorder_colors; alpha = ic_recorder_alpha; width = ic_recorder_width; height = ic_recorder_height;
  } else if (app.action == AppAction::Settings) {
    colors = ic_settings_colors; alpha = ic_settings_alpha; width = ic_settings_width; height = ic_settings_height;
  } else if (app.action == AppAction::Workout) {
    colors = ic_workout_colors; alpha = ic_workout_alpha; width = ic_workout_width; height = ic_workout_height;
  } else if (app.action == AppAction::Files) {
    colors = ic_files_colors; alpha = ic_files_alpha; width = ic_files_width; height = ic_files_height;
  } else if (app.action == AppAction::Browser) {
    colors = ic_browser_colors; alpha = ic_browser_alpha; width = ic_browser_width; height = ic_browser_height;
  } else if (app.action == AppAction::UsbDrive) {
    colors = ic_usb_colors; alpha = ic_usb_alpha; width = ic_usb_width; height = ic_usb_height;
  }
  if (colors && alpha) {
    UiKit::drawRgba565Icon(canvas, centerX - width / 2, centerY - height / 2,
                           width, height, colors, alpha, app.circleColor);
  }
}

bool HomeScreen::buildPageStrip() {
  if (_pageStrip.created()) _pageStrip.deleteSprite();
  _pageStripReady = false;
  const int16_t stripWidth = static_cast<int16_t>(_display.width() * pageCount());
  _pageStrip.setColorDepth(16);
  if (!_pageStrip.createSprite(stripWidth, PageContentHeight)) {
    Serial.printf("[Home] page strip allocation failed: %dx%d, psram=%u, largest=%u\n",
                  stripWidth, PageContentHeight, ESP.getFreePsram(), ESP.getMaxAllocPsram());
    return false;
  }
  if (!_pageFrame.created()) {
    _pageFrame.setColorDepth(16);
    if (!_pageFrame.createSprite(_display.width(), PageFrameHeight)) {
      Serial.printf("[Home] page frame allocation failed: %ux%d, psram=%u, largest=%u\n",
                    _display.width(), PageFrameHeight,
                    ESP.getFreePsram(), ESP.getMaxAllocPsram());
      _pageStrip.deleteSprite();
      return false;
    }
    _pageFrame.fillSprite(TFT_BLACK);
    _renderedScroll = INT32_MIN;
  }

  _pageStripReady = true;
  _pageStrip.fillSprite(TFT_BLACK);
  for (uint8_t index = 0; index < _appCount; ++index) {
    const AppTile &app = _apps[index];
    const int16_t pageOffset = static_cast<int16_t>(index / PageSize) * _display.width();
    drawAppTileTo(_pageStrip, app, pageOffset, -StatusBarHeight);
  }
  return true;
}

void HomeScreen::destroyPageStrip() {
  if (_pageFrame.created()) _pageFrame.deleteSprite();
  if (_pageStrip.created()) _pageStrip.deleteSprite();
  _pageStripReady = false;
  _renderedScroll = INT32_MIN;
}

void HomeScreen::restoreCachedPage() {
  if (!_display.isReady() || !_pageStripReady || !_pageFrame.created()) return;
  _visibleScroll = static_cast<int32_t>(_page) * _display.width();
  if (_renderedScroll != _visibleScroll) renderPageViewport(true);
  else _pageFrame.pushSprite(0, StatusBarHeight);
  if (_display.height() > PageFrameBottom) {
    _display.tft().fillRect(0, PageFrameBottom, _display.width(),
                            _display.height() - PageFrameBottom, TFT_BLACK);
  }
  drawStatusBar();
}

void HomeScreen::renderPageViewport(bool force) {
  if (!_pageStripReady || !_display.isReady()) return;
  const uint32_t nowMs = millis();
  if (!force && _renderedScroll == _visibleScroll) return;
  if (!force && nowMs - _lastAnimationFrameMs < AnimationFrameMs) return;

  uint16_t *frame = static_cast<uint16_t *>(_pageFrame.getPointer());
  const uint16_t *strip = static_cast<const uint16_t *>(_pageStrip.getPointer());
  if (!frame || !strip) return;
  const int32_t displayWidth = _display.width();
  const int32_t stripWidth = displayWidth * pageCount();
  const int32_t maximum = static_cast<int32_t>(pageCount() - 1) * _display.width();
  int32_t sourceX = _visibleScroll;
  int16_t destinationX = 0;
  int16_t copyWidth = static_cast<int16_t>(displayWidth);
  if (_visibleScroll < 0) {
    destinationX = static_cast<int16_t>(min<int32_t>(displayWidth, -_visibleScroll));
    sourceX = 0;
    copyWidth = static_cast<int16_t>(displayWidth - destinationX);
  } else if (_visibleScroll > maximum) {
    const int16_t overscroll = static_cast<int16_t>(min<int32_t>(
        displayWidth, _visibleScroll - maximum));
    sourceX = maximum + overscroll;
    copyWidth = static_cast<int16_t>(displayWidth - overscroll);
  }

  if (copyWidth < displayWidth) {
    memset(frame, 0, static_cast<size_t>(displayWidth) * PageContentHeight * sizeof(uint16_t));
  }
  if (copyWidth > 0 && sourceX >= 0 && sourceX + copyWidth <= stripWidth) {
    for (int16_t row = 0; row < PageContentHeight; ++row) {
      memcpy(frame + static_cast<size_t>(row) * displayWidth + destinationX,
             strip + static_cast<size_t>(row) * stripWidth + sourceX,
             static_cast<size_t>(copyWidth) * sizeof(uint16_t));
    }
  }
  drawPageIndicatorTo(_pageFrame, StatusBarHeight);
  _pageFrame.pushSprite(0, StatusBarHeight);
  _renderedScroll = _visibleScroll;
  _lastAnimationFrameMs = nowMs;
}

void HomeScreen::updateCatalogTiles() {
  uint64_t previous[MaxHomeApps] = {};
  const uint8_t previousCount = _appCount;
  const uint8_t previousPages = pageCount();
  const uint8_t previousPage = _page;
  for (uint8_t index = 0; index < previousCount; ++index) {
    previous[index] = tileSignature(_apps[index]);
  }

  AppCatalogLoader::clearCatalog(_catalog);
  _catalog = _pendingCatalog;
  _pendingCatalog = AppCatalogLoader::Catalog{};
  rebuildApps();

  bool changed[MaxHomeApps] = {};
  bool anyChanged = false;
  const uint8_t comparedCount = max(previousCount, _appCount);
  for (uint8_t index = 0; index < comparedCount; ++index) {
    const bool differs = index >= previousCount || index >= _appCount ||
        previous[index] != tileSignature(_apps[index]);
    changed[index] = differs;
    anyChanged |= differs;
  }
  const uint8_t currentPages = pageCount();
  const bool pageStructureChanged = previousPages != currentPages;
  if (!anyChanged && !pageStructureChanged) return;

  if (_pageStripReady) {
    if (pageStructureChanged) {
      if (!buildPageStrip()) {
        draw();
        return;
      }
    } else {
      for (uint8_t index = 0; index < comparedCount; ++index) {
        if (changed[index]) redrawStripSlot(index);
      }
    }

    if (previousPage != _page) {
      _visibleScroll = static_cast<int32_t>(_page) * _display.width();
      _renderedScroll = INT32_MIN;
      renderPageViewport(true);
      return;
    }
    for (uint8_t index = 0; index < comparedCount; ++index) {
      if (changed[index]) presentSlot(index);
    }
    if (pageStructureChanged) presentPageIndicator();
    return;
  }

  for (uint8_t index = 0; index < comparedCount; ++index) {
    if (!changed[index] || index / PageSize != _page) continue;
    int16_t x = 0;
    int16_t y = 0;
    int16_t width = 0;
    int16_t height = 0;
    slotBounds(index, x, y, width, height);
    _display.tft().fillRect(x, y + StatusBarHeight, width, height, TFT_BLACK);
    if (index < _appCount) drawAppTileTo(_display.tft(), _apps[index], 0, 0);
  }
  if (pageStructureChanged) drawPageIndicator();
}

void HomeScreen::redrawStripSlot(uint8_t index) {
  if (!_pageStripReady || index >= MaxHomeApps) return;
  int16_t x = 0;
  int16_t y = 0;
  int16_t width = 0;
  int16_t height = 0;
  slotBounds(index, x, y, width, height);
  const int16_t pageOffset = static_cast<int16_t>(index / PageSize) * _display.width();
  _pageStrip.fillRect(pageOffset + x, y, width, height, TFT_BLACK);
  if (index < _appCount) {
    drawAppTileTo(_pageStrip, _apps[index], pageOffset, -StatusBarHeight);
  }
}

void HomeScreen::presentSlot(uint8_t index) {
  if (!_pageStripReady || !_pageFrame.created() || index / PageSize != _page) return;
  int16_t x = 0;
  int16_t y = 0;
  int16_t width = 0;
  int16_t height = 0;
  slotBounds(index, x, y, width, height);
  uint16_t *frame = static_cast<uint16_t *>(_pageFrame.getPointer());
  const uint16_t *strip = static_cast<const uint16_t *>(_pageStrip.getPointer());
  if (!frame || !strip) return;
  const int32_t displayWidth = _display.width();
  const int32_t stripWidth = displayWidth * pageCount();
  const int32_t sourceX = static_cast<int32_t>(_page) * displayWidth + x;
  for (int16_t row = 0; row < height; ++row) {
    memcpy(frame + static_cast<size_t>(y + row) * displayWidth + x,
           strip + static_cast<size_t>(y + row) * stripWidth + sourceX,
           static_cast<size_t>(width) * sizeof(uint16_t));
  }
  _pageFrame.pushSprite(x, y + StatusBarHeight, x, y, width, height);
}

void HomeScreen::presentPageIndicator() {
  if (!_pageFrame.created()) {
    drawPageIndicator();
    return;
  }
  constexpr int16_t indicatorTop = 436;
  constexpr int16_t indicatorHeight = 25;
  const int16_t sourceY = indicatorTop - StatusBarHeight;
  drawPageIndicatorTo(_pageFrame, StatusBarHeight);
  _pageFrame.pushSprite(0, indicatorTop, 0, sourceY,
                        _display.width(), indicatorHeight);
}

void HomeScreen::slotBounds(uint8_t index, int16_t &x, int16_t &y,
                            int16_t &width, int16_t &height) const {
  const int16_t columnWidth = _display.width() / AppColumns;
  const uint8_t slot = index % PageSize;
  const uint8_t column = slot % AppColumns;
  const uint8_t row = slot / AppColumns;
  x = column * columnWidth;
  y = 16 + row * 116;
  width = column == AppColumns - 1 ? _display.width() - x : columnWidth;
  height = 116;
}

uint64_t HomeScreen::tileSignature(const AppTile &app) const {
  uint64_t hash = 1469598103934665603ULL;
  hashBytes(hash, app.label, strlen(app.label) + 1);
  hashBytes(hash, app.externalId, strlen(app.externalId) + 1);
  hashBytes(hash, &app.action, sizeof(app.action));
  hashBytes(hash, &app.circleColor, sizeof(app.circleColor));
  const uint8_t hasArtwork = app.iconColors && app.iconAlpha ? 1 : 0;
  hashBytes(hash, &hasArtwork, sizeof(hasArtwork));
  if (hasArtwork) {
    constexpr size_t pixels = static_cast<size_t>(AppCatalogLoader::IconWidth) *
        AppCatalogLoader::IconHeight;
    hashBytes(hash, app.iconColors, pixels * sizeof(uint16_t));
    hashBytes(hash, app.iconAlpha, pixels);
  }
  return hash;
}

uint8_t HomeScreen::pageCount() const {
  return max<uint8_t>(1, (_appCount + PageSize - 1) / PageSize);
}

int8_t HomeScreen::appAt(uint16_t x, uint16_t y) const {
  const uint8_t first = _page * PageSize;
  const uint8_t end = min<uint8_t>(_appCount, first + PageSize);
  for (uint8_t index = first; index < end; ++index) {
    const AppTile &app = _apps[index];
    if (x >= app.x && x < app.x + app.w && y >= app.y && y < app.y + app.h) {
      return static_cast<int8_t>(index);
    }
  }
  return -1;
}

void HomeScreen::activateApp(const AppTile &app) {
  switch (app.action) {
    case AppAction::AiAssistant: _navigator.push(ScreenId::Assistant); break;
    case AppAction::Files: _navigator.push(ScreenId::Files); break;
    case AppAction::Recorder: _navigator.push(ScreenId::VoiceRecorder); break;
    case AppAction::Settings: _navigator.push(ScreenId::Settings); break;
    case AppAction::Browser: _navigator.push(ScreenId::Browser); break;
    case AppAction::UsbDrive: _navigator.push(ScreenId::UsbDrive); break;
    case AppAction::Workout: _navigator.push(ScreenId::Workout); break;
    case AppAction::MacRemote: _navigator.push(ScreenId::MacRemote); break;
    case AppAction::External:
      _navigator.push(ScreenId::ExternalApp, ScreenData::externalApp(app.externalId));
      break;
  }
}

void HomeScreen::updateGesture(uint16_t x, uint16_t y) {
  if (!_gestureActive) return;
  const uint32_t nowMs = millis();
  if (static_cast<int16_t>(x) != _velocityEndX) {
    if (nowMs - _velocityStartMs > VelocityWindowMs) {
      _velocityStartX = _velocityEndX;
      _velocityStartMs = _velocityEndMs;
    }
    _velocityEndX = static_cast<int16_t>(x);
    _velocityEndMs = nowMs;
  }

  const int32_t deltaX = static_cast<int32_t>(x) - _gestureStartX;
  const int32_t deltaY = static_cast<int32_t>(y) - _gestureStartY;
  const int32_t absX = abs(deltaX);
  const int32_t absY = abs(deltaY);
  if (absX > TapSlop || absY > TapSlop) _gestureCancelled = true;
  if (!_horizontalDrag) {
    if (absX <= TapSlop || absX <= absY) return;
    _horizontalDrag = true;
    _gestureCancelled = true;
    _pressedIndex = -1;
  }

  int32_t nextScroll = _gestureBaseScroll - deltaX;
  const int32_t maximum = static_cast<int32_t>(pageCount() - 1) * _display.width();
  if (nextScroll < 0) nextScroll /= 3;
  else if (nextScroll > maximum) nextScroll = maximum + (nextScroll - maximum) / 3;
  if (_visibleScroll == nextScroll) return;
  _visibleScroll = nextScroll;
  renderPageViewport();
}

void HomeScreen::finishGesture(uint32_t nowMs) {
  if (!_gestureActive) return;
  if (_horizontalDrag) {
    const int32_t width = _display.width();
    const int32_t maximum = static_cast<int32_t>(pageCount() - 1) * width;
    const int32_t clampedBase = min<int32_t>(maximum, max<int32_t>(0, _gestureBaseScroll));
    const uint8_t originPage = static_cast<uint8_t>((clampedBase + width / 2) / width);
    const int32_t deltaX = static_cast<int32_t>(_velocityEndX) - _gestureStartX;
    const int16_t velocity = releaseVelocity(nowMs);

    int8_t direction = 0;
    if (_horizontalDrag && abs(deltaX) >= SwipeThreshold) direction = deltaX < 0 ? 1 : -1;
    else if (_horizontalDrag && abs(velocity) >= FlickVelocity) direction = velocity < 0 ? 1 : -1;

    int16_t target = originPage + direction;
    target = max<int16_t>(0, min<int16_t>(pageCount() - 1, target));
    resetGesture();
    beginSettle(static_cast<uint8_t>(target), nowMs);
    return;
  }

  const int8_t pressedIndex = _pressedIndex;
  const bool shouldActivate = !_gestureCancelled && !_filesHoldTriggered &&
      pressedIndex >= 0 && pressedIndex < _appCount;
  resetGesture();
  if (shouldActivate) activateApp(_apps[pressedIndex]);
}

void HomeScreen::beginSettle(uint8_t targetPage, uint32_t nowMs) {
  targetPage = min<uint8_t>(pageCount() - 1, targetPage);
  const int32_t targetScroll = static_cast<int32_t>(targetPage) * _display.width();
  // Hit testing follows the accepted destination immediately; only the
  // visual viewport remains in flight during the short settle animation.
  _page = targetPage;
  if (!_pageStripReady) {
    _visibleScroll = targetScroll;
    draw();
    return;
  }
  const uint32_t distance = static_cast<uint32_t>(abs(targetScroll - _visibleScroll));
  if (distance == 0) {
    _settling = false;
    renderPageViewport(true);
    return;
  }
  _settleFromScroll = _visibleScroll;
  _settleToScroll = targetScroll;
  _settleStartedMs = nowMs;
  const uint32_t scaled = distance * SettleDurationMs / _display.width();
  _settleDurationMs = static_cast<uint16_t>(
      min<uint32_t>(SettleDurationMs, max<uint32_t>(80, scaled)));
  _settling = true;
}

void HomeScreen::updateSettle(uint32_t nowMs, bool force) {
  if (!_settling) return;
  const uint32_t elapsed = nowMs - _settleStartedMs;
  if (elapsed >= _settleDurationMs) {
    _visibleScroll = _settleToScroll;
    _page = static_cast<uint8_t>(_settleToScroll / _display.width());
    _settling = false;
    renderPageViewport(true);
    return;
  }

  const float progress = static_cast<float>(elapsed) / _settleDurationMs;
  const float remaining = 1.0f - progress;
  const float eased = 1.0f - remaining * remaining * remaining;
  _visibleScroll = _settleFromScroll + static_cast<int32_t>(
      (_settleToScroll - _settleFromScroll) * eased);
  renderPageViewport(force);
}

int16_t HomeScreen::releaseVelocity(uint32_t nowMs) const {
  if (_velocityEndMs <= _velocityStartMs ||
      nowMs - _velocityEndMs > VelocityWindowMs) return 0;
  const int32_t elapsed = _velocityEndMs - _velocityStartMs;
  const int32_t velocity = (static_cast<int32_t>(_velocityEndX) - _velocityStartX) *
      1000 / elapsed;
  return static_cast<int16_t>(max<int32_t>(INT16_MIN, min<int32_t>(INT16_MAX, velocity)));
}

void HomeScreen::cancelMotion() {
  _settling = false;
  resetGesture();
}

void HomeScreen::resetGesture() {
  _gestureActive = false;
  _gestureCancelled = false;
  _horizontalDrag = false;
  _filesHoldTriggered = false;
  _pressedIndex = -1;
}

String HomeScreen::clockText(const struct tm &localTime) const {
  char buffer[6];
  snprintf(buffer, sizeof(buffer), "%02d:%02d", localTime.tm_hour, localTime.tm_min);
  return String(buffer);
}

String HomeScreen::truncateLabel(const String &label, int16_t maxWidth) {
  return UiText::ellipsize(label, maxWidth, UiText::Size::Compact);
}

void HomeScreen::setSdState(bool mounted) {
  if (_sdMounted == mounted) return;
  _sdMounted = mounted;
  drawSdIcon();
}

void HomeScreen::setWifiState(bool connected) {
  if (_wifiConnected == connected) return;
  _wifiConnected = connected;
  drawWifiIcon();
}

void HomeScreen::setBatteryVoltage(float voltage) {
  const String previousText = UiKit::batteryVoltageText(_batteryVoltage);
  const uint16_t previousFill = UiKit::batteryVoltageColor(_batteryVoltage);
  const String nextText = UiKit::batteryVoltageText(voltage);
  const uint16_t nextFill = UiKit::batteryVoltageColor(voltage);
  if (previousText == nextText && previousFill == nextFill) {
    _batteryVoltage = voltage;
    return;
  }
  _batteryVoltage = voltage;
  drawBattery();
}

void HomeScreen::refreshSdState() { _sdMounted = _sdCard.isMounted(); }

void HomeScreen::handleSdCardEvent(SdCardEvent event) {
  if (event == SdCardEvent::Mounted) {
    setSdState(true);
    _catalogLoader.requestScan();
  } else if (event == SdCardEvent::Removed || event == SdCardEvent::MountFailed) {
    setSdState(false);
    _catalogLoader.cancel();
    cancelMotion();
    AppCatalogLoader::clearCatalog(_pendingCatalog);
    updateCatalogTiles();
  }
}
