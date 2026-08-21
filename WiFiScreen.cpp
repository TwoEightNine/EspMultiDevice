#include "WiFiScreen.h"

#include "Navigator.h"

namespace {
const int16_t TopBarHeight = 54;
const int16_t ContentPadding = 16;
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
    _switchBounds{0, SwitchY, SwitchW, SwitchH},
    _forgetBounds{ContentPadding, 208, 200, 42},
    _rescanBounds{ContentPadding, 278, 120, 38} {
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

  if (point.x >= 10 && point.x < 90 && point.y >= 10 && point.y < 44) {
    _navigator.pop();
    return;
  }

  if (contains(_switchBounds, point.x, point.y)) {
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

  const int16_t firstRowY = 148;
  const int16_t rowHeight = 30;
  for (uint8_t i = 0; i < _wifi.networkCount() && i < VisibleNetworkCount; i++) {
    Rect row = {ContentPadding, (int16_t)(firstRowY + i * rowHeight),
                _display.width() - 2 * ContentPadding, rowHeight};
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
    tft.setTextColor(MutedColor, TFT_BLACK);
    tft.drawString("Wi-Fi is disabled", ContentPadding, 132, 2);
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
  TFT_eSPI &tft = _display.tft();
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawLine(18, 27, 34, 19, TFT_WHITE);
  tft.drawLine(18, 27, 34, 35, TFT_WHITE);
  tft.drawLine(18, 27, 48, 27, TFT_WHITE);
  tft.drawString("Wi-Fi", 108, 16, 4);
}

void WiFiScreen::drawEnabledRow() {
  TFT_eSPI &tft = _display.tft();
  _switchBounds = {_display.width() - ContentPadding - SwitchW,
                   SwitchY, SwitchW, SwitchH};
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Enabled", ContentPadding, 85, 2);
  tft.setTextDatum(TL_DATUM);
  drawSwitch();
  tft.drawFastHLine(ContentPadding, TopBarHeight + 52,
                    _display.width() - 2 * ContentPadding, TFT_DARKGREY);
}

void WiFiScreen::drawSwitch() {
  TFT_eSPI &tft = _display.tft();
  uint16_t fill = _wifi.isEnabled() ? ActiveColor : TFT_DARKGREY;
  tft.fillRoundRect(_switchBounds.x, _switchBounds.y,
                    _switchBounds.w, _switchBounds.h, 16, fill);
  tft.drawRoundRect(_switchBounds.x, _switchBounds.y,
                    _switchBounds.w, _switchBounds.h, 16, TFT_WHITE);
  int16_t knobX = _wifi.isEnabled() ? _switchBounds.x + _switchBounds.w - 17
                                    : _switchBounds.x + 17;
  tft.fillCircle(knobX, _switchBounds.y + _switchBounds.h / 2, 11, TFT_WHITE);
}

void WiFiScreen::drawConnected() {
  TFT_eSPI &tft = _display.tft();
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(_wifi.ssid(), ContentPadding, 126, 2);
  tft.setTextColor(MutedColor, TFT_BLACK);
  tft.drawString(_wifi.networkType(), ContentPadding, 150, 2);
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
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Available networks", ContentPadding, 120, 2);
  if (_wifi.error().length() > 0) {
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString(_wifi.error(), ContentPadding + 220, 123, 1);
  }

  const int16_t firstRowY = 148;
  const int16_t rowHeight = 30;
  uint8_t shown = _wifi.networkCount() < VisibleNetworkCount
                    ? _wifi.networkCount() : VisibleNetworkCount;
  for (uint8_t i = 0; i < shown; i++) {
    const WiFiNetworkInfo &network = _wifi.networkAt(i);
    int16_t y = firstRowY + i * rowHeight;
    tft.drawFastHLine(ContentPadding, y + rowHeight - 1,
                      _display.width() - 2 * ContentPadding, TFT_DARKGREY);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(network.ssid, ContentPadding, y + 4, 2);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(MutedColor, TFT_BLACK);
    String details = String(WiFiController::securityType(network.authMode)) +
                     " " + String(network.rssi) + "dBm";
    tft.drawString(details, _display.width() - ContentPadding, y + 4, 1);
    tft.setTextDatum(TL_DATUM);
  }

  if (shown == 0) {
    tft.setTextColor(MutedColor, TFT_BLACK);
    tft.drawString(_wifi.error().length() > 0 ? _wifi.error() : "No networks found",
                   ContentPadding, 158, 2);
  }

  _rescanBounds = {ContentPadding, shown == 0 ? 208 : (int16_t)(firstRowY + shown * rowHeight + 8),
                   120, 38};
  drawButton(_rescanBounds, "Rescan", ButtonColor);
}

void WiFiScreen::drawButton(const Rect &bounds, const char *label, uint16_t fill) {
  TFT_eSPI &tft = _display.tft();
  tft.fillRoundRect(bounds.x, bounds.y, bounds.w, bounds.h, 6, fill);
  tft.drawRoundRect(bounds.x, bounds.y, bounds.w, bounds.h, 6, TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, fill);
  tft.drawString(label, bounds.x + bounds.w / 2,
                 bounds.y + bounds.h / 2, 2);
  tft.setTextDatum(TL_DATUM);
}

void WiFiScreen::drawLoader(const char *message, uint32_t nowMs) {
  TFT_eSPI &tft = _display.tft();
  static const char *dots[] = {"", ".", "..", "..."};
  const uint8_t frame = (nowMs / 500) % 4;
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(String(message) + dots[frame], _display.width() / 2, 156, 2);
  tft.setTextDatum(TL_DATUM);
}

bool WiFiScreen::contains(const Rect &bounds, int16_t x, int16_t y) const {
  return x >= bounds.x && x < bounds.x + bounds.w &&
         y >= bounds.y && y < bounds.y + bounds.h;
}

bool WiFiScreen::isLoaderVisible() const {
  return _wifi.isScanning() || _wifi.state() == WiFiConnectionState::Connecting;
}
