#include "WiFiScreen.h"

#include "../../app/Navigator.h"
#include "../../ui/UiKit.h"

namespace {
const int16_t ContentPadding = UiKit::ContentPadding;
const int16_t EnabledRowY = UiKit::AppBarHeight;
const int16_t FirstNetworkRowY = 124;
const uint16_t ButtonColor = TFT_DARKGREY;
const uint16_t ActiveColor = TFT_BLUE;
const uint16_t MutedColor = TFT_LIGHTGREY;
}

WiFiScreen::WiFiScreen(Navigator &navigator,
                       TouchController &touch,
                       DisplayController &display,
                       WiFiController &wifi)
  : Screen(navigator, touch, display),
    _wifi(wifi),
    _lastTouchDown(false),
    _forceDraw(true),
    _lastDrawMs(0),
    _lastState(WiFiConnectionState::Disabled),
    _lastEnabled(false),
    _lastScanning(false),
    _lastNetworkCount(0),
    _lastError(),
    _lastSsid(),
    _selectedSsid(),
    _switchBounds{0, EnabledRowY, SwitchW, SwitchH},
    _forgetBounds{ContentPadding, 174, 200, 42},
    _rescanBounds{ContentPadding, 276, 120, 38} {
}

void WiFiScreen::onEnter() {
  _lastTouchDown = false;
  _forceDraw = true;
  if (_wifi.isEnabled() && !_wifi.isConnected() && !_wifi.isScanning() &&
      _wifi.networkCount() == 0 && _wifi.state() != WiFiConnectionState::Failed) {
    _wifi.startScan();
  }
  draw();
}

void WiFiScreen::onResult(const ScreenResult &result) {
  if (result.kind != ScreenResultKind::Text || _selectedSsid.length() == 0) {
    return;
  }

  _wifi.connect(_selectedSsid, result.text);
  _selectedSsid = "";
  _forceDraw = true;
  draw();
}

void WiFiScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) {
    _lastTouchDown = false;
    return;
  }
  if (_lastTouchDown) {
    return;
  }
  _lastTouchDown = true;

  if (UiKit::navigationIconContains(point.x, point.y)) {
    _navigator.pop();
    return;
  }

  const Rect enabledRow = {0, EnabledRowY, _display.width(), UiKit::ListCellHeight};
  if (contains(enabledRow, point.x, point.y)) {
    _wifi.setEnabled(!_wifi.isEnabled());
    _forceDraw = true;
    draw();
    return;
  }

  if (!_wifi.isEnabled()) {
    return;
  }

  if (_wifi.isConnected() && contains(_forgetBounds, point.x, point.y)) {
    _wifi.forgetNetwork();
    _forceDraw = true;
    draw();
    return;
  }

  if (!_wifi.isConnected() && !isLoaderVisible() &&
      contains(_rescanBounds, point.x, point.y)) {
    _wifi.startScan();
    _forceDraw = true;
    draw();
    return;
  }

  if (_wifi.isConnected() || isLoaderVisible()) {
    return;
  }

  const int16_t firstRowY = FirstNetworkRowY;
  const int16_t rowHeight = UiKit::ListCellHeight;
  for (uint8_t i = 0; i < _wifi.networkCount() && i < VisibleNetworkCount; i++) {
    Rect row = {0, (int16_t)(firstRowY + i * rowHeight),
                _display.width(), rowHeight};
    if (contains(row, point.x, point.y)) {
      _selectedSsid = _wifi.networkAt(i).ssid;
      _navigator.push(ScreenId::TextInput,
                      ScreenData::textInput("Wi-Fi password", true));
      return;
    }
  }
}

void WiFiScreen::tick(uint32_t nowMs) {
  bool changed = _forceDraw ||
                 _lastState != _wifi.state() ||
                 _lastEnabled != _wifi.isEnabled() ||
                 _lastScanning != _wifi.isScanning() ||
                 _lastNetworkCount != _wifi.networkCount() ||
                 _lastError != _wifi.error() ||
                 _lastSsid != _wifi.ssid();
  if (isLoaderVisible() && nowMs - _lastDrawMs >= 500) {
    changed = true;
  }
  if (changed) {
    draw();
  }
}

void WiFiScreen::draw() {
  if (!_display.isReady()) {
    return;
  }

  uint32_t nowMs = millis();
  _display.tft().fillScreen(TFT_BLACK);
  drawTopBar();
  drawEnabledRow();

  if (!_wifi.isEnabled()) {
    TFT_eSPI &tft = _display.tft();
    UiText::draw(tft, "Wi-Fi is disabled", ContentPadding, 120,
                 MutedColor, TFT_BLACK, UiText::Size::Compact);
  } else if (_wifi.isConnected()) {
    drawConnected();
  } else {
    drawDisconnected(nowMs);
  }

  _lastState = _wifi.state();
  _lastEnabled = _wifi.isEnabled();
  _lastScanning = _wifi.isScanning();
  _lastNetworkCount = _wifi.networkCount();
  _lastError = _wifi.error();
  _lastSsid = _wifi.ssid();
  _lastDrawMs = nowMs;
  _forceDraw = false;
}

void WiFiScreen::drawTopBar() {
  UiKit::drawTopAppBar(_display.tft(), _display.width(), F("Wi-Fi"));
}

void WiFiScreen::drawEnabledRow() {
  _switchBounds = {_display.width() - ContentPadding - SwitchW,
                   (int16_t)(EnabledRowY + (UiKit::ListCellHeight - SwitchH) / 2),
                   SwitchW, SwitchH};
  UiKit::drawListCell(_display.tft(), _display.width(), EnabledRowY, F("Enabled"));
  drawSwitch();
}

void WiFiScreen::drawSwitch() {
  UiKit::drawToggle(_display.tft(), _switchBounds.x, _switchBounds.y,
                    _switchBounds.w, _switchBounds.h, _wifi.isEnabled(),
                    ActiveColor, TFT_BLACK);
}

void WiFiScreen::drawConnected() {
  TFT_eSPI &tft = _display.tft();
  UiText::draw(tft, _wifi.ssid(), ContentPadding, 116, TFT_WHITE, TFT_BLACK,
               UiText::Size::Content);
  UiText::draw(tft, _wifi.networkType(), ContentPadding, 140, MutedColor, TFT_BLACK,
               UiText::Size::Compact);
  drawButton(_forgetBounds, "Forget network", ButtonColor);
}

void WiFiScreen::drawDisconnected(uint32_t nowMs) {
  if (isLoaderVisible()) {
    const char *message = _wifi.state() == WiFiConnectionState::Connecting
                            ? "Connecting..." : "Searching for networks...";
    drawLoader(message, nowMs);
    return;
  }

  drawNetworkList();
}

void WiFiScreen::drawNetworkList() {
  TFT_eSPI &tft = _display.tft();
  UiText::draw(tft, "Available networks", ContentPadding, 108, TFT_WHITE, TFT_BLACK,
               UiText::Size::Compact);
  if (_wifi.error().length() > 0) {
    UiText::draw(tft, _wifi.error(), ContentPadding + 220, 111, TFT_YELLOW, TFT_BLACK,
                 UiText::Size::Compact);
  }

  const int16_t firstRowY = FirstNetworkRowY;
  const int16_t rowHeight = UiKit::ListCellHeight;
  uint8_t shown = _wifi.networkCount() < VisibleNetworkCount
                    ? _wifi.networkCount() : VisibleNetworkCount;
  for (uint8_t i = 0; i < shown; i++) {
    const WiFiNetworkInfo &network = _wifi.networkAt(i);
    int16_t y = firstRowY + i * rowHeight;
    String details = String(WiFiController::securityType(network.authMode)) +
                     " " + String(network.rssi) + "dBm";
    UiKit::drawListCell(tft, _display.width(), y, network.ssid, details);
  }

  if (shown == 0) {
    UiText::draw(tft, _wifi.error().length() > 0 ? _wifi.error() : "No networks found",
                 ContentPadding, 148, MutedColor, TFT_BLACK, UiText::Size::Compact);
  }

  _rescanBounds = {ContentPadding, shown == 0 ? 192 : (int16_t)(firstRowY + shown * rowHeight + 8),
                   120, 38};
  drawButton(_rescanBounds, "Rescan", ButtonColor);
}

void WiFiScreen::drawButton(const Rect &bounds, const char *label, uint16_t fill) {
  UiKit::drawFilledButton(_display.tft(), bounds.x, bounds.y, bounds.w, bounds.h,
                          label, fill, TFT_WHITE, TFT_BLACK);
}

void WiFiScreen::drawLoader(const char *message, uint32_t nowMs) {
  TFT_eSPI &tft = _display.tft();
  static const char *dots[] = {"", ".", "..", "..."};
  const uint8_t frame = (nowMs / 500) % 4;
  UiText::draw(tft, String(message) + dots[frame], _display.width() / 2, 156,
               TFT_WHITE, TFT_BLACK, UiText::Size::Compact, MC_DATUM);
}

bool WiFiScreen::contains(const Rect &bounds, int16_t x, int16_t y) const {
  return x >= bounds.x && x < bounds.x + bounds.w &&
         y >= bounds.y && y < bounds.y + bounds.h;
}

bool WiFiScreen::isLoaderVisible() const {
  return _wifi.isScanning() || _wifi.state() == WiFiConnectionState::Connecting;
}
