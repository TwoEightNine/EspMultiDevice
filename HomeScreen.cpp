#include "HomeScreen.h"

#include "Navigator.h"
#include "UiText.h"
#include "icons.h"

#define COLOR_BATTERY_GREEN 0x2c89
#define COLOR_BATTERY_YELLOW 0xa54a
#define COLOR_BATTERY_RED 0xa1e5

namespace {
const uint16_t StatusActiveColor = TFT_WHITE;
const uint16_t StatusInactiveColor = TFT_DARKGREY;
const uint16_t AppIconColor = TFT_WHITE;
}

HomeScreen::HomeScreen(Navigator &navigator,
                       TouchController &touch,
                       DisplayController &display,
                       SdCardController &sdCard,
                       WiFiController &wifi,
                       TimeController &time)
  : Screen(navigator, touch, display),
    _sdCard(sdCard),
    _wifi(wifi),
    _time(time),
    _sdMounted(false),
    _wifiConnected(false),
    _batteryPercent(BatteryStubPercent),
    _lastTouchDown(false),
    _filesPressPending(false),
    _filesHoldTriggered(false),
    _filesPressedMs(0),
    _lastClockCheckMs(0xffffffffUL),
    _lastClockText("") {
  _apps[0] = { "Files", AppAction::Files, TFT_BLUE, nullptr, 0, 0, 0, 0 };
  _apps[1] = { "Recorder", AppAction::Recorder, TFT_RED, nullptr, 0, 0, 0, 0 };
  _apps[2] = { "Settings", AppAction::Settings, TFT_DARKGREEN, nullptr, 0, 0, 0, 0 };
  _apps[3] = { "Browser", AppAction::Browser, TFT_CYAN, nullptr, 0, 0, 0, 0 };
  _apps[4] = { "Workout", AppAction::Workout, TFT_ORANGE, nullptr, 0, 0, 0, 0 };
}

void HomeScreen::onEnter() {
  refreshSdState();
  _wifiConnected = _wifi.isConnected();
  _lastClockCheckMs = 0xffffffffUL;
  _lastClockText = "";
  _filesPressPending = false;
  _filesHoldTriggered = false;
  draw();
  // Do not run a read/write probe on every return to Home.  Besides creating
  // needless SD traffic, it used to run in the screen-enter call path and
  // could make the outgoing screen appear frozen.
}

void HomeScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) {
    _lastTouchDown = false;
    if (_filesPressPending && !_filesHoldTriggered) _navigator.push(ScreenId::Files);
    _filesPressPending = false;
    return;
  }

  if (_lastTouchDown) {
    return;
  }
  _lastTouchDown = true;

  for (uint8_t i = 0; i < AppCount; i++) {
    AppTile &app = _apps[i];
    if (point.x < app.x || point.x >= app.x + app.w ||
        point.y < app.y || point.y >= app.y + app.h) {
      continue;
    }

    if (app.action == AppAction::Files) {
      _filesPressPending = true;
      _filesHoldTriggered = false;
      _filesPressedMs = millis();
    } else if (app.action == AppAction::Recorder) {
      _navigator.push(ScreenId::VoiceRecorder);
    } else if (app.action == AppAction::Settings) {
      _navigator.push(ScreenId::Settings);
    } else if (app.action == AppAction::Browser) {
      _navigator.push(ScreenId::Browser);
    } else if (app.action == AppAction::Workout) {
      _navigator.push(ScreenId::Workout);
    }
    return;
  }
}

void HomeScreen::onResult(const ScreenResult &result) {
  if (result.kind != ScreenResultKind::Text || result.text.length() != 4) return;
  for (uint8_t i = 0; i < 4; i++) if (result.text[i] < '0' || result.text[i] > '9') return;
  const uint16_t pin = (uint16_t)result.text.toInt();
  _navigator.push(ScreenId::Files, ScreenData::secretFiles(pin));
}

void HomeScreen::tick(uint32_t nowMs) {
  const TouchPoint heldPoint = _touch.point();
  const AppTile &files = _apps[0];
  const bool holdingFiles = heldPoint.touched && heldPoint.x >= files.x && heldPoint.x < files.x + files.w &&
                            heldPoint.y >= files.y && heldPoint.y < files.y + files.h;
  if (_filesPressPending && !_filesHoldTriggered && holdingFiles && nowMs - _filesPressedMs >= 1000) {
    _filesHoldTriggered = true;
    _filesPressPending = false;
    _navigator.push(ScreenId::TextInput, ScreenData::pinInput("Secret PIN"));
    return;
  }
  if (_lastClockCheckMs == 0xffffffffUL || nowMs - _lastClockCheckMs >= 1000UL) {
    drawClock(false);
    _lastClockCheckMs = nowMs;
  }

  handleSdCardEvent(_sdCard.takeEvent());
  setWifiState(_wifi.isConnected());
}

void HomeScreen::draw() {
  if (!_display.isReady()) {
    return;
  }

  layoutApps();
  _display.tft().fillScreen(TFT_BLACK);
  drawStatusBar();
  drawApps();
}

void HomeScreen::layoutApps() {
  int16_t width = _display.width();
  int16_t columnWidth = width / AppColumns;
  int16_t tileTop = StatusBarHeight + 16;
  int16_t tileHeight = 116;

  for (uint8_t i = 0; i < AppCount; i++) {
    uint8_t column = i % AppColumns;
    uint8_t row = i / AppColumns;
    _apps[i].x = column * columnWidth;
    _apps[i].y = tileTop + row * tileHeight;
    _apps[i].w = column == AppColumns - 1 ? width - _apps[i].x : columnWidth;
    _apps[i].h = tileHeight;
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
  if (!_display.isReady()) {
    return;
  }

  struct tm local = {};
  String text = _time.localTime(local) ? clockText(local) : "--:--";
  if (!force && text == _lastClockText) {
    return;
  }

  TFT_eSPI &tft = _display.tft();
  tft.fillRect(0, 0, 72, StatusBarHeight, TFT_BLACK);
  UiText::draw(tft, text, 8, StatusBarHeight / 2, TFT_WHITE, TFT_BLACK,
               UiText::Size::Compact, ML_DATUM);
  _lastClockText = text;
}

void HomeScreen::drawSdIcon() {
  TFT_eSPI &tft = _display.tft();
  const int16_t x = _display.width() - 82;
  const int16_t y = 4;
  uint16_t color = _sdMounted ? StatusActiveColor : StatusInactiveColor;
  tft.fillRect(x, y, 16, 16, TFT_BLACK);
  tft.drawXBitmap(x, y, ic_sd, 16, 16, color);
}

void HomeScreen::drawWifiIcon() {
  TFT_eSPI &tft = _display.tft();
  const int16_t x = _display.width() - 58;
  const int16_t y = 4;
  uint16_t color = _wifiConnected ? StatusActiveColor : StatusInactiveColor;
  tft.fillRect(x, y, 16, 16, TFT_BLACK);
  tft.drawXBitmap(x, y, ic_wifi, 16, 16, color);
}

void HomeScreen::drawBattery() {
  TFT_eSPI &tft = _display.tft();
  const int16_t x = _display.width() - 38;
  const int16_t y = 4;
  uint16_t fill = _batteryPercent <= 20 ? COLOR_BATTERY_RED : (_batteryPercent <= 50 ? COLOR_BATTERY_YELLOW : COLOR_BATTERY_GREEN);
  tft.fillRect(x, y, 38, 16, TFT_BLACK);
  tft.drawRect(x, y, 32, 16, TFT_WHITE);
  tft.fillRect(x + 32, y + 5, 3, 6, TFT_WHITE);
  tft.fillRect(x + 1, y + 1, 30, 14, fill);
  UiText::draw(tft, String(_batteryPercent), x + 16, y + 8, TFT_WHITE, fill,
               UiText::Size::Compact, MC_DATUM);
}

void HomeScreen::drawApps() {
  TFT_eSPI &tft = _display.tft();
  for (uint8_t i = 0; i < AppCount; i++) {
    const AppTile &app = _apps[i];
    int16_t centerX = app.x + app.w / 2;
    int16_t centerY = app.y + 48;
    tft.fillCircle(centerX, centerY, IconDiameter / 2, app.circleColor);
    drawAppIcon(app, centerX, centerY);

    String label = truncateLabel(String(app.label), app.w - 12);
    UiText::draw(tft, label, centerX, app.y + 94, TFT_WHITE, TFT_BLACK,
                 UiText::Size::Compact, TC_DATUM);
  }
}

void HomeScreen::drawAppIcon(const AppTile &app, int16_t centerX, int16_t centerY) {
  (void)app;
  (void)centerX;
  (void)centerY;
  // app icon: 64x64, 1 bit per pixel, colored dynamically at runtime
  // TFT_eSPI &tft = _display.tft();
  // int16_t x = centerX - IconBitmapSize / 2;
  // int16_t y = centerY - IconBitmapSize / 2;
  // tft.drawXBitmap(x, y, app.bitmap, IconBitmapSize, IconBitmapSize, AppIconColor, app.circleColor);
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
  if (_sdMounted == mounted) {
    return;
  }
  _sdMounted = mounted;
  drawSdIcon();
}

void HomeScreen::setWifiState(bool connected) {
  if (_wifiConnected == connected) {
    return;
  }
  _wifiConnected = connected;
  drawWifiIcon();
}

void HomeScreen::refreshSdState() {
  _sdMounted = _sdCard.isMounted();
}

void HomeScreen::handleSdCardEvent(SdCardEvent event) {
  if (event == SdCardEvent::Mounted) {
    setSdState(true);
  } else if (event == SdCardEvent::Removed || event == SdCardEvent::MountFailed) {
    setSdState(false);
  }
}
