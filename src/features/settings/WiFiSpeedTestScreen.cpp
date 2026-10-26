#include "WiFiSpeedTestScreen.h"

#include <WiFi.h>

#include "../../app/Navigator.h"
#include "../../ui/UiKit.h"

namespace {
constexpr int16_t ModeTop = UiKit::AppBarHeight + 8;
constexpr int16_t ModeHeight = 40;
constexpr int16_t DetailsTop = 104;
constexpr int16_t MetricsTop = 154;
constexpr int16_t ProgressTop = 304;
constexpr int16_t StatusTop = 340;
constexpr int16_t ButtonTop = 366;
constexpr int16_t ButtonHeight = 48;
constexpr uint16_t ActiveColor = TFT_BLUE;
constexpr uint16_t DisabledColor = TFT_DARKGREY;
constexpr uint16_t MutedColor = TFT_LIGHTGREY;
}

WiFiSpeedTestScreen::WiFiSpeedTestScreen(Navigator &navigator,
                                         TouchController &touch,
                                         DisplayController &display,
                                         WiFiController &wifi,
                                         SdCardController &sdCard)
    : Screen(navigator, touch, display),
      _wifi(wifi),
      _sdCard(sdCard),
      _wifiWorker(wifi),
      _sdWorker(sdCard),
      _wifiSnapshot{},
      _sdSnapshot{},
      _mode(Mode::WiFi),
      _lastTouchDown(false),
      _closePending(false),
      _lastConnected(false),
      _lastSdMounted(false),
      _lastWorkerActive(false),
      _lastRssi(0),
      _lastSignalDrawMs(0),
      _wifiModeBounds{UiKit::ContentPadding, ModeTop, 137, ModeHeight},
      _sdModeBounds{167, ModeTop, 137, ModeHeight},
      _buttonBounds{UiKit::ContentPadding, ButtonTop,
                    static_cast<int16_t>(display.width() -
                                         UiKit::ContentPadding * 2),
                    ButtonHeight} {}

void WiFiSpeedTestScreen::onEnter() {
  _lastTouchDown = false;
  _closePending = false;
  _mode = Mode::WiFi;
  _wifiWorker.begin();
  _sdWorker.begin();
  _wifiSnapshot = _wifiWorker.snapshot();
  _sdSnapshot = _sdWorker.snapshot();
  _lastConnected = _wifi.isConnected();
  _lastSdMounted = _sdCard.isMounted();
  _lastWorkerActive = workerActive();
  _lastRssi = _lastConnected ? WiFi.RSSI() : 0;
  _lastSignalDrawMs = millis();
  draw();
}

bool WiFiSpeedTestScreen::releaseResources() {
  if (!_closePending) {
    _closePending = true;
    _wifiWorker.cancel();
    _sdWorker.cancel();
  }
  const bool wifiEnded = _wifiWorker.end();
  const bool sdEnded = _sdWorker.end();
  if (!wifiEnded || !sdEnded) return false;
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

  if (!workerActive()) {
    Mode selected = _mode;
    if (contains(_wifiModeBounds, point.x, point.y)) selected = Mode::WiFi;
    if (contains(_sdModeBounds, point.x, point.y)) selected = Mode::Sd;
    if (selected != _mode) {
      _mode = selected;
      _lastConnected = _wifi.isConnected();
      _lastSdMounted = _sdCard.isMounted();
      if (_lastConnected) _lastRssi = WiFi.RSSI();
      _lastWorkerActive = false;
      drawModeSelector();
      drawModeContent();
      return;
    }
  }

  if (!contains(_buttonBounds, point.x, point.y)) return;
  if (_mode == Mode::WiFi) {
    if (_wifiWorker.active()) _wifiWorker.cancel();
    else if (_wifi.isConnected()) _wifiWorker.start();
    refreshWiFi(_wifiWorker.snapshot(), _wifi.isConnected(),
                _wifi.isConnected() ? WiFi.RSSI() : 0,
                _wifiWorker.active());
  } else {
    if (_sdWorker.active()) _sdWorker.cancel();
    else if (_sdCard.isMounted()) _sdWorker.start();
    refreshSd(_sdWorker.snapshot(), _sdCard.isMounted(), _sdWorker.active());
  }
}

void WiFiSpeedTestScreen::tick(uint32_t nowMs) {
  if (_closePending) {
    _navigator.pop();
    return;
  }

  if (_mode == Mode::WiFi) {
    const WiFiSpeedTestSnapshot current = _wifiWorker.snapshot();
    const bool connected = _wifi.isConnected();
    const bool active = _wifiWorker.active();
    int32_t rssi = _lastRssi;
    if (connected && nowMs - _lastSignalDrawMs >= 1000) {
      rssi = WiFi.RSSI();
      _lastSignalDrawMs = nowMs;
    }
    if (current.revision != _wifiSnapshot.revision ||
        connected != _lastConnected || active != _lastWorkerActive ||
        (connected && rssi != _lastRssi)) {
      refreshWiFi(current, connected, connected ? rssi : 0, active);
    }
  } else {
    const SdSpeedTestSnapshot current = _sdWorker.snapshot();
    const bool mounted = _sdCard.isMounted();
    const bool active = _sdWorker.active();
    if (current.revision != _sdSnapshot.revision ||
        mounted != _lastSdMounted || active != _lastWorkerActive) {
      refreshSd(current, mounted, active);
    }
  }
}

void WiFiSpeedTestScreen::draw() {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  tft.fillScreen(TFT_BLACK);
  UiKit::drawTopAppBar(tft, _display.width(), F("Speed test"));
  drawModeSelector();
  drawModeContent();
}

void WiFiSpeedTestScreen::drawModeSelector() {
  TFT_eSPI &tft = _display.tft();
  UiKit::drawFilledButton(tft, _wifiModeBounds.x, _wifiModeBounds.y,
                         _wifiModeBounds.w, _wifiModeBounds.h, "Wi-Fi",
                         _mode == Mode::WiFi ? ActiveColor : DisabledColor,
                         TFT_WHITE, TFT_BLACK);
  UiKit::drawFilledButton(tft, _sdModeBounds.x, _sdModeBounds.y,
                         _sdModeBounds.w, _sdModeBounds.h, "SD",
                         _mode == Mode::Sd ? ActiveColor : DisabledColor,
                         TFT_WHITE, TFT_BLACK);
}

void WiFiSpeedTestScreen::drawModeContent() {
  TFT_eSPI &tft = _display.tft();
  tft.fillRect(0, DetailsTop - 4, _display.width(),
               _display.height() - DetailsTop + 4, TFT_BLACK);
  drawConnection();
  drawMetrics();
  drawProgress(true);
  drawAction();
}

void WiFiSpeedTestScreen::refreshWiFi(
    const WiFiSpeedTestSnapshot &snapshot, bool connected, int32_t rssi,
    bool active) {
  if (!_display.isReady()) return;
  const WiFiSpeedTestSnapshot previous = _wifiSnapshot;
  const bool connectionChanged = connected != _lastConnected;
  const bool signalChanged = connected && rssi != _lastRssi;
  const bool activeChanged = active != _lastWorkerActive;
  const bool phaseChanged = snapshot.phase != previous.phase;
  const bool progressChanged = snapshot.progress != previous.progress;
  const bool statusChanged = strncmp(snapshot.status, previous.status,
                                     sizeof(snapshot.status)) != 0;

  _wifiSnapshot = snapshot;
  _lastConnected = connected;
  _lastWorkerActive = active;
  _lastRssi = rssi;

  if (connectionChanged || signalChanged) drawConnection();
  if (_wifiSnapshot.latencyMs != previous.latencyMs) drawMetric(0);
  if (_wifiSnapshot.downloadMbps != previous.downloadMbps) drawMetric(1);
  if (_wifiSnapshot.uploadMbps != previous.uploadMbps) drawMetric(2);
  if (progressChanged || statusChanged || phaseChanged)
    drawProgress(false, previous.progress);
  if (connectionChanged || activeChanged || phaseChanged) drawActionButton();
}

void WiFiSpeedTestScreen::refreshSd(const SdSpeedTestSnapshot &snapshot,
                                    bool mounted, bool active) {
  if (!_display.isReady()) return;
  const SdSpeedTestSnapshot previous = _sdSnapshot;
  const bool mountChanged = mounted != _lastSdMounted;
  const bool activeChanged = active != _lastWorkerActive;
  const bool phaseChanged = snapshot.phase != previous.phase;
  const bool progressChanged = snapshot.progress != previous.progress;
  const bool statusChanged = strncmp(snapshot.status, previous.status,
                                     sizeof(snapshot.status)) != 0;

  _sdSnapshot = snapshot;
  _lastSdMounted = mounted;
  _lastWorkerActive = active;

  if (mountChanged) drawConnection();
  if (_sdSnapshot.writeMiBps != previous.writeMiBps) drawMetric(0);
  if (_sdSnapshot.readMiBps != previous.readMiBps) drawMetric(1);
  if (_sdSnapshot.bulkReadMiBps != previous.bulkReadMiBps) drawMetric(2);
  if (progressChanged || statusChanged || phaseChanged)
    drawProgress(false, previous.progress);
  if (mountChanged || activeChanged || phaseChanged) drawActionButton();
}

void WiFiSpeedTestScreen::drawConnection() {
  TFT_eSPI &tft = _display.tft();
  tft.fillRect(0, DetailsTop - 2, _display.width(), 48, TFT_BLACK);
  if (_mode == Mode::WiFi) {
    if (_wifi.isConnected()) {
      String connection = UiKit::ellipsize(_wifi.ssid(),
          _display.width() - 112, UiText::Size::Content);
      UiText::draw(tft, connection, UiKit::ContentPadding, DetailsTop,
                   TFT_WHITE, TFT_BLACK, UiText::Size::Content);
      String signal = String(_lastRssi) + F(" dBm");
      UiText::draw(tft, signal, _display.width() - UiKit::ContentPadding,
                   DetailsTop + 2, TFT_CYAN, TFT_BLACK,
                   UiText::Size::Compact, TR_DATUM);
    } else {
      UiText::draw(tft, F("Connect to Wi-Fi first"), UiKit::ContentPadding,
                   DetailsTop, TFT_YELLOW, TFT_BLACK, UiText::Size::Content);
    }
    UiText::draw(tft, F("Internet estimate over current Wi-Fi"),
                 UiKit::ContentPadding, DetailsTop + 26,
                 MutedColor, TFT_BLACK, UiText::Size::Compact);
  } else {
    UiText::draw(tft, _sdCard.isMounted() ? F("SD card mounted")
                                         : F("Insert an SD card first"),
                 UiKit::ContentPadding, DetailsTop,
                 _sdCard.isMounted() ? TFT_WHITE : TFT_YELLOW,
                 TFT_BLACK, UiText::Size::Content);
    UiText::draw(tft, F("Verified 4 MiB write and read test"),
                 UiKit::ContentPadding, DetailsTop + 26,
                 MutedColor, TFT_BLACK, UiText::Size::Compact);
  }
}

void WiFiSpeedTestScreen::drawMetrics() {
  for (uint8_t index = 0; index < 3; ++index) drawMetric(index);
}

void WiFiSpeedTestScreen::drawMetric(uint8_t index) {
  TFT_eSPI &tft = _display.tft();
  const int16_t y = MetricsTop + UiKit::ListCellHeight * index;
  if (_mode == Mode::WiFi) {
    if (index == 0) {
      UiKit::drawListCell(tft, _display.width(), y, F("HTTP latency"),
                          metricText(_wifiSnapshot.latencyMs, 0, " ms"));
    } else if (index == 1) {
      UiKit::drawListCell(tft, _display.width(), y, F("Download"),
                          metricText(_wifiSnapshot.downloadMbps, 1, " Mbps"));
    } else {
      UiKit::drawListCell(tft, _display.width(), y, F("Upload"),
                          metricText(_wifiSnapshot.uploadMbps, 1, " Mbps"));
    }
  } else {
    if (index == 0) {
      UiKit::drawListCell(tft, _display.width(), y, F("Write"),
                          metricText(_sdSnapshot.writeMiBps, 2, " MiB/s"));
    } else if (index == 1) {
      UiKit::drawListCell(tft, _display.width(), y, F("Read 4 KiB"),
                          metricText(_sdSnapshot.readMiBps, 2, " MiB/s"));
    } else {
      UiKit::drawListCell(tft, _display.width(), y, F("Read 32 KiB"),
                          metricText(_sdSnapshot.bulkReadMiBps, 2, " MiB/s"));
    }
  }
}

void WiFiSpeedTestScreen::drawProgress(bool full, uint8_t previousProgress) {
  TFT_eSPI &tft = _display.tft();
  const int16_t width = _display.width() - UiKit::ContentPadding * 2;
  if (full) {
    UiKit::drawProgressBar(tft, UiKit::ContentPadding, ProgressTop,
                          width, 16, progress(), false, ActiveColor,
                          TFT_DARKGREY, TFT_BLACK);
  } else if (progress() != previousProgress) {
    UiKit::updateProgressBar(tft, UiKit::ContentPadding, ProgressTop,
                            width, 16, previousProgress, progress(),
                            ActiveColor, TFT_BLACK);
  }
  tft.fillRect(0, StatusTop - 12, _display.width(), 24, TFT_BLACK);
  const char *message = status();
  if (!message[0]) message = "Ready";
  UiText::draw(tft, UiKit::ellipsize(message,
                                    _display.width() -
                                        UiKit::ContentPadding * 2,
                                    UiText::Size::Compact),
               _display.width() / 2, StatusTop, TFT_WHITE, TFT_BLACK,
               UiText::Size::Compact, MC_DATUM);
}

void WiFiSpeedTestScreen::drawAction() {
  drawActionButton();
  TFT_eSPI &tft = _display.tft();
  if (_mode == Mode::WiFi) {
    UiText::draw(tft, F("Cloudflare traffic, up to 10.5 MiB"),
                 _display.width() / 2, 438, MutedColor, TFT_BLACK,
                 UiText::Size::Compact, MC_DATUM);
  } else {
    UiText::draw(tft, F("Temporary test file is removed"),
                 _display.width() / 2, 438, MutedColor, TFT_BLACK,
                 UiText::Size::Compact, MC_DATUM);
  }
  UiText::draw(tft, F("Results are not saved"), _display.width() / 2, 458,
               MutedColor, TFT_BLACK, UiText::Size::Compact, MC_DATUM);
}

void WiFiSpeedTestScreen::drawActionButton() {
  TFT_eSPI &tft = _display.tft();
  const bool ready = _mode == Mode::WiFi ? _wifi.isConnected()
                                         : _sdCard.isMounted();
  const bool enabled = ready || workerActive();
  UiKit::drawFilledButton(tft, _buttonBounds.x, _buttonBounds.y,
                          _buttonBounds.w, _buttonBounds.h,
                          buttonLabel(), enabled ? ActiveColor : DisabledColor,
                          enabled ? TFT_WHITE : TFT_LIGHTGREY, TFT_BLACK);
}

bool WiFiSpeedTestScreen::workerActive() const {
  return _mode == Mode::WiFi ? _wifiWorker.active() : _sdWorker.active();
}

uint8_t WiFiSpeedTestScreen::progress() const {
  return _mode == Mode::WiFi ? _wifiSnapshot.progress : _sdSnapshot.progress;
}

const char *WiFiSpeedTestScreen::status() const {
  return _mode == Mode::WiFi ? _wifiSnapshot.status : _sdSnapshot.status;
}

bool WiFiSpeedTestScreen::contains(const Rect &bounds, int16_t x,
                                   int16_t y) const {
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
  if (_mode == Mode::WiFi) {
    if (_wifiWorker.active() &&
        _wifiSnapshot.phase == WiFiSpeedTestPhase::Cancelled)
      return "Cancelling...";
    if (_wifiWorker.active()) return "Cancel";
    if (!_wifi.isConnected()) return "Wi-Fi required";
    if (_wifiSnapshot.phase == WiFiSpeedTestPhase::Complete)
      return "Run again";
    if (_wifiSnapshot.phase == WiFiSpeedTestPhase::Error ||
        _wifiSnapshot.phase == WiFiSpeedTestPhase::Cancelled)
      return "Retry";
    return "Start test";
  }

  if (_sdWorker.active() && _sdSnapshot.phase == SdSpeedTestPhase::Cancelled)
    return "Cancelling...";
  if (_sdWorker.active()) return "Cancel";
  if (!_sdCard.isMounted()) return "SD card required";
  if (_sdSnapshot.phase == SdSpeedTestPhase::Complete) return "Run again";
  if (_sdSnapshot.phase == SdSpeedTestPhase::Error ||
      _sdSnapshot.phase == SdSpeedTestPhase::Cancelled) return "Retry";
  return "Start test";
}
