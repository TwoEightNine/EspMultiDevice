#include "HomeScreen.h"

#include <ctype.h>
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
    _catalogRevision(0),
    _sdMounted(false),
    _wifiConnected(false),
    _batteryVoltage(0.0f),
    _gestureActive(false),
    _gestureCancelled(false),
    _swipeHandled(false),
    _filesHoldTriggered(false),
    _pressedIndex(-1),
    _gestureStartX(0),
    _gestureStartY(0),
    _gestureStartedMs(0),
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

bool HomeScreen::releaseResources() {
  _catalogLoader.stop();
  if (!_catalogLoader.releaseResources()) return false;
  AppCatalogLoader::clearCatalog(_catalog);
  return true;
}

void HomeScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) {
    if (_gestureActive && !_gestureCancelled && !_swipeHandled &&
        !_filesHoldTriggered && _pressedIndex >= 0 && _pressedIndex < _appCount) {
      activateApp(_apps[_pressedIndex]);
    }
    resetGesture();
    return;
  }

  if (!_gestureActive) {
    _gestureActive = true;
    _gestureStartX = point.x;
    _gestureStartY = point.y;
    _gestureStartedMs = millis();
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

  if (!_gestureActive && _catalogLoader.takeCatalog(_catalog, _catalogRevision)) {
    rebuildApps();
    draw();
  }
}

void HomeScreen::draw() {
  if (!_display.isReady()) return;
  layoutApps();
  _display.tft().fillScreen(TFT_BLACK);
  drawStatusBar();
  drawApps();
  drawPageIndicator();
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
  const uint8_t pageCount = max<uint8_t>(1, (_appCount + PageSize - 1) / PageSize);
  if (_page >= pageCount) _page = pageCount - 1;
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
    const AppTile &app = _apps[index];
    const int16_t centerX = app.x + app.w / 2;
    const int16_t centerY = app.y + 48;
    tft.fillSmoothCircle(centerX, centerY, IconDiameter / 2,
                         app.circleColor, TFT_BLACK);
    drawAppIcon(app, centerX, centerY);
    const String label = truncateLabel(String(app.label), app.w - 12);
    UiText::draw(tft, label, centerX, app.y + 94, TFT_WHITE, TFT_BLACK,
                 UiText::Size::Compact, TC_DATUM);
  }
}

void HomeScreen::drawPageIndicator() {
  const uint8_t pageCount = max<uint8_t>(1, (_appCount + PageSize - 1) / PageSize);
  if (pageCount <= 1) return;
  TFT_eSPI &tft = _display.tft();
  constexpr int16_t spacing = 14;
  const int16_t startX = (_display.width() - (pageCount - 1) * spacing) / 2;
  constexpr int16_t y = 448;
  for (uint8_t index = 0; index < pageCount; ++index) {
    const uint16_t color = index == _page ? TFT_WHITE : TFT_DARKGREY;
    tft.fillSmoothCircle(startX + index * spacing, y, index == _page ? 4 : 3,
                         color, TFT_BLACK);
  }
}

void HomeScreen::drawAppIcon(const AppTile &app, int16_t centerX, int16_t centerY) {
  if (app.action == AppAction::External) {
    if (app.iconColors && app.iconAlpha) {
      UiKit::drawRgba565IconFromMemory(
          _display.tft(), centerX - AppCatalogLoader::IconWidth / 2,
          centerY - AppCatalogLoader::IconHeight / 2,
          AppCatalogLoader::IconWidth, AppCatalogLoader::IconHeight,
          app.iconColors, app.iconAlpha, app.circleColor);
    } else {
      UiKit::drawAppsIcon(_display.tft(), centerX, centerY, TFT_WHITE, app.circleColor);
    }
    return;
  }
  if (app.action == AppAction::MacRemote) {
    UiKit::drawMacRemoteIcon(_display.tft(), centerX, centerY, TFT_WHITE,
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
    UiKit::drawRgba565Icon(_display.tft(), centerX - width / 2, centerY - height / 2,
                           width, height, colors, alpha, app.circleColor);
  }
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
  if (!_gestureActive || _swipeHandled) return;
  const int16_t deltaX = static_cast<int16_t>(x) - static_cast<int16_t>(_gestureStartX);
  const int16_t deltaY = static_cast<int16_t>(y) - static_cast<int16_t>(_gestureStartY);
  const int16_t absX = abs(deltaX);
  const int16_t absY = abs(deltaY);
  if (absX > TapSlop || absY > TapSlop) _gestureCancelled = true;
  if (absX < SwipeThreshold || absX <= absY) return;

  const uint8_t pageCount = max<uint8_t>(1, (_appCount + PageSize - 1) / PageSize);
  if (deltaX < 0 && _page + 1 < pageCount) ++_page;
  else if (deltaX > 0 && _page > 0) --_page;
  _swipeHandled = true;
  _gestureCancelled = true;
  draw();
}

void HomeScreen::resetGesture() {
  _gestureActive = false;
  _gestureCancelled = false;
  _swipeHandled = false;
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
    resetGesture();
    AppCatalogLoader::clearCatalog(_catalog);
    rebuildApps();
    draw();
  }
}
