#include "HomeScreen.h"

#include "Navigator.h"

#define COLOR_BATTERY_GREEN 0x2c89
#define COLOR_BATTERY_YELLOW 0xa54a
#define COLOR_BATTERY_RED 0xa1e5

namespace {
const uint32_t MillisPerMinute = 60000UL;
const uint16_t StatusActiveColor = TFT_WHITE;
const uint16_t StatusInactiveColor = TFT_DARKGREY;
const uint16_t AppIconColor = TFT_WHITE;
}

HomeScreen::HomeScreen(Navigator &navigator,
                       TouchController &touch,
                       DisplayController &display,
                       SdCardController &sdCard)
  : Screen(navigator, touch, display),
    _sdCard(sdCard),
    _sdMounted(false),
    _wifiConnected(false),
    _batteryPercent(BatteryStubPercent),
    _lastTouchDown(false),
    _lastClockMinute(0xffffffffUL),
    _lastClockText("") {
  _apps[0] = { "Files", AppAction::Files, TFT_BLUE, nullptr, 0, 0, 0, 0 };
  _apps[1] = { "Recorder", AppAction::Recorder, TFT_RED, nullptr, 0, 0, 0, 0 };
  _apps[2] = { "Settings", AppAction::Settings, TFT_DARKGREEN, nullptr, 0, 0, 0, 0 };
}

void HomeScreen::onEnter() {
  refreshSdState();
  _lastClockMinute = 0xffffffffUL;
  _lastClockText = "";
  draw();
  // Retain the existing storage diagnostic, but keep it out of the home UI.
  runSdSelfTest();
}

void HomeScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) {
    _lastTouchDown = false;
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

    if (app.action == AppAction::Recorder) {
      _navigator.push(ScreenId::VoiceRecorder);
    }
    // Files and Settings are intentional no-op stubs until their screens exist.
    return;
  }
}

void HomeScreen::tick(uint32_t nowMs) {
  uint32_t minute = nowMs / MillisPerMinute;
  if (minute != _lastClockMinute) {
    drawClock(false);
  }

  handleSdCardEvent(_sdCard.update());
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
  int16_t columnWidth = width / AppCount;
  int16_t tileTop = StatusBarHeight + 16;
  int16_t tileHeight = 150;

  for (uint8_t i = 0; i < AppCount; i++) {
    _apps[i].x = i * columnWidth;
    _apps[i].y = tileTop;
    _apps[i].w = (i == AppCount - 1) ? width - _apps[i].x : columnWidth;
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

  uint32_t minute = millis() / MillisPerMinute;
  String text = clockText(millis());
  if (!force && minute == _lastClockMinute && text == _lastClockText) {
    return;
  }

  TFT_eSPI &tft = _display.tft();
  tft.fillRect(0, 0, 72, StatusBarHeight, TFT_BLACK);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(text, 8, StatusBarHeight / 2, 2);
  tft.setTextDatum(TL_DATUM);
  _lastClockMinute = minute;
  _lastClockText = text;
}

void HomeScreen::drawSdIcon() {
  // sd card icon
  // const int16_t x = _display.width() - 82;
  // const int16_t y = 4;
  // uint16_t color = _sdMounted ? TFT_WHITE : TFT_DARKGREY;
  // tft.pushImage(x, y, 16, 16, sdCardIcon, color);
}

void HomeScreen::drawWifiIcon() {
  // wifi icon
  // const int16_t x = _display.width() - 58;
  // const int16_t y = 4;
  // uint16_t color = _wifiConnected ? TFT_WHITE : TFT_DARKGREY;
  // tft.pushImage(x, y, 16, 16, wifiIcon, color);
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
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, fill);
  tft.drawNumber(_batteryPercent, x + 16, y + 8, 1);
  tft.setTextDatum(TL_DATUM);
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
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(label, centerX, app.y + 94, 2);
    tft.setTextDatum(TL_DATUM);
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

String HomeScreen::clockText(uint32_t nowMs) const {
  uint32_t totalMinutes = nowMs / MillisPerMinute;
  uint8_t hours = (totalMinutes / 60) % 24;
  uint8_t minutes = totalMinutes % 60;
  char buffer[6];
  snprintf(buffer, sizeof(buffer), "%02u:%02u", hours, minutes);
  return String(buffer);
}

String HomeScreen::truncateLabel(const String &label, int16_t maxWidth) {
  TFT_eSPI &tft = _display.tft();
  if (tft.textWidth(label, 2) <= maxWidth) {
    return label;
  }

  String result = label;
  while (result.length() > 3 && tft.textWidth(result + "...", 2) > maxWidth) {
    result.remove(result.length() - 1);
  }
  return result + "...";
}

void HomeScreen::setSdState(bool mounted) {
  if (_sdMounted == mounted) {
    return;
  }
  _sdMounted = mounted;
  drawSdIcon();
}

void HomeScreen::refreshSdState() {
  _sdMounted = _sdCard.isMounted();
}

void HomeScreen::runSdSelfTest() {
  if (!_sdCard.isMounted()) {
    return;
  }

  bool passed = _sdCard.runReadWriteTest();
  setSdState(passed);
}

void HomeScreen::handleSdCardEvent(SdCardEvent event) {
  if (event == SdCardEvent::Mounted) {
    setSdState(true);
    runSdSelfTest();
  } else if (event == SdCardEvent::Removed || event == SdCardEvent::MountFailed) {
    setSdState(false);
  }
}
