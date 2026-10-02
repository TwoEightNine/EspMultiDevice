#include "WiFiSpeedTestScreen.h"

#include <WiFi.h>

#include "../../app/Navigator.h"
#include "../../ui/UiKit.h"

namespace {
constexpr int16_t ConnectionTop = UiKit::AppBarHeight + 12;
constexpr int16_t MetricsTop = 126;
constexpr int16_t ProgressTop = 264;
constexpr int16_t StatusTop = 300;
constexpr int16_t ButtonTop = 350;
constexpr int16_t ButtonHeight = 48;
constexpr uint16_t ActiveColor = TFT_BLUE;
constexpr uint16_t DisabledColor = TFT_DARKGREY;
constexpr uint16_t MutedColor = TFT_LIGHTGREY;
}

WiFiSpeedTestScreen::WiFiSpeedTestScreen(Navigator &navigator,
                                         TouchController &touch,
                                         DisplayController &display,
                                         WiFiController &wifi)
    : Screen(navigator, touch, display),
      _wifi(wifi),
      _worker(wifi),
      _snapshot{},
      _lastTouchDown(false),
      _closePending(false),
      _lastConnected(false),
      _lastRssi(0),
      _lastSignalDrawMs(0),
      _buttonBounds{UiKit::ContentPadding, ButtonTop,
                    static_cast<int16_t>(display.width() - UiKit::ContentPadding * 2),
                    ButtonHeight} {}

void WiFiSpeedTestScreen::onEnter() {
  _lastTouchDown = false;
  _closePending = false;
  _worker.begin();
  _snapshot = _worker.snapshot();
  _lastConnected = _wifi.isConnected();
  _lastRssi = _lastConnected ? WiFi.RSSI() : 0;
  _lastSignalDrawMs = millis();
  draw();
}

bool WiFiSpeedTestScreen::releaseResources() {
  if (!_closePending) {
    _closePending = true;
    _worker.cancel();
  }
  if (!_worker.end()) return false;
  _closePending = false;
  return true;
}

void WiFiSpeedTestScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) {
    _lastTouchDown = false;
    return;
  }
  if (_lastTouchDown || _closePending) return;
  _lastTouchDown = true;

  if (UiKit::navigationIconContains(point.x, point.y)) {
    _navigator.pop();
    return;
  }
  if (!contains(_buttonBounds, point.x, point.y)) return;

  if (_worker.active()) {
    _worker.cancel();
  } else if (_wifi.isConnected()) {
    _worker.start();
  }
  _snapshot = _worker.snapshot();
  draw();
}

void WiFiSpeedTestScreen::tick(uint32_t nowMs) {
  if (_closePending) {
    _navigator.pop();
    return;
  }

  const WiFiSpeedTestSnapshot current = _worker.snapshot();
  const bool connected = _wifi.isConnected();
  int32_t rssi = _lastRssi;
  if (connected && nowMs - _lastSignalDrawMs >= 1000) {
    rssi = WiFi.RSSI();
    _lastSignalDrawMs = nowMs;
  }
  if (current.revision != _snapshot.revision || connected != _lastConnected ||
      (connected && rssi != _lastRssi)) {
    _snapshot = current;
    _lastConnected = connected;
    _lastRssi = connected ? rssi : 0;
    draw();
  }
}

void WiFiSpeedTestScreen::draw() {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  tft.fillScreen(TFT_BLACK);
  UiKit::drawTopAppBar(tft, _display.width(), F("Wi-Fi speed test"));
  drawConnection();
  drawMetrics();
  drawProgress();
  drawAction();
}

void WiFiSpeedTestScreen::drawConnection() {
  TFT_eSPI &tft = _display.tft();
  if (_wifi.isConnected()) {
    String connection = UiKit::ellipsize(_wifi.ssid(), _display.width() - 112,
                                         UiText::Size::Content);
    UiText::draw(tft, connection, UiKit::ContentPadding, ConnectionTop,
                 TFT_WHITE, TFT_BLACK, UiText::Size::Content);
    String signal = String(_lastRssi) + F(" dBm");
    UiText::draw(tft, signal, _display.width() - UiKit::ContentPadding,
                 ConnectionTop + 2, TFT_CYAN, TFT_BLACK,
                 UiText::Size::Compact, TR_DATUM);
  } else {
    UiText::draw(tft, F("Connect to Wi-Fi first"), UiKit::ContentPadding,
                 ConnectionTop, TFT_YELLOW, TFT_BLACK, UiText::Size::Content);
  }
  UiText::draw(tft, F("Internet estimate over the current Wi-Fi"),
               UiKit::ContentPadding, ConnectionTop + 28,
               MutedColor, TFT_BLACK, UiText::Size::Compact);
}

void WiFiSpeedTestScreen::drawMetrics() {
  TFT_eSPI &tft = _display.tft();
  UiKit::drawListCell(tft, _display.width(), MetricsTop, F("HTTP latency"),
                      metricText(_snapshot.latencyMs, 0, " ms"));
  UiKit::drawListCell(tft, _display.width(), MetricsTop + UiKit::ListCellHeight,
                      F("Download"), metricText(_snapshot.downloadMbps, 1, " Mbps"));
  UiKit::drawListCell(tft, _display.width(), MetricsTop + UiKit::ListCellHeight * 2,
                      F("Upload"), metricText(_snapshot.uploadMbps, 1, " Mbps"));
}

void WiFiSpeedTestScreen::drawProgress() {
  TFT_eSPI &tft = _display.tft();
  UiKit::drawProgressBar(tft, UiKit::ContentPadding, ProgressTop,
                         _display.width() - UiKit::ContentPadding * 2, 16,
                         _snapshot.progress, false, ActiveColor,
                         TFT_DARKGREY, TFT_BLACK);
  const char *status = _snapshot.status[0] ? _snapshot.status : "Ready";
  UiText::draw(tft, UiKit::ellipsize(status,
                                    _display.width() - UiKit::ContentPadding * 2,
                                    UiText::Size::Compact),
               _display.width() / 2, StatusTop, TFT_WHITE, TFT_BLACK,
               UiText::Size::Compact, MC_DATUM);
}

void WiFiSpeedTestScreen::drawAction() {
  TFT_eSPI &tft = _display.tft();
  const bool enabled = _wifi.isConnected() || _worker.active();
  UiKit::drawFilledButton(tft, _buttonBounds.x, _buttonBounds.y,
                          _buttonBounds.w, _buttonBounds.h,
                          buttonLabel(), enabled ? ActiveColor : DisabledColor,
                          enabled ? TFT_WHITE : TFT_LIGHTGREY, TFT_BLACK);
  UiText::draw(tft, F("Cloudflare test traffic, up to 10.5 MiB"),
               _display.width() / 2, 430, MutedColor, TFT_BLACK,
               UiText::Size::Compact, MC_DATUM);
  UiText::draw(tft, F("Results are not saved"), _display.width() / 2, 452,
               MutedColor, TFT_BLACK, UiText::Size::Compact, MC_DATUM);
}

bool WiFiSpeedTestScreen::contains(const Rect &bounds, int16_t x, int16_t y) const {
  return x >= bounds.x && x < bounds.x + bounds.w &&
         y >= bounds.y && y < bounds.y + bounds.h;
}

String WiFiSpeedTestScreen::metricText(float value, uint8_t decimals,
                                       const char *suffix) const {
  if (value < 0.0f) return F("--");
  String text(value, static_cast<unsigned int>(decimals));
  text += suffix;
  return text;
}

const char *WiFiSpeedTestScreen::buttonLabel() const {
  if (_worker.active()) return "Cancel";
  if (!_wifi.isConnected()) return "Wi-Fi required";
  if (_snapshot.phase == WiFiSpeedTestPhase::Complete) return "Run again";
  if (_snapshot.phase == WiFiSpeedTestPhase::Error ||
      _snapshot.phase == WiFiSpeedTestPhase::Cancelled) return "Retry";
  return "Start test";
}
