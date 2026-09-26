#include "HomeScreen.h"

#include "../../app/Navigator.h"
#include "../../ui/UiKit.h"
#include "../../ui/UiText.h"
#include "../../ui/icons.h"

#define COLOR_BATTERY_GREEN 0x2c89
#define COLOR_BATTERY_YELLOW 0xa54a
#define COLOR_BATTERY_RED 0xa1e5

namespace {
const uint16_t StatusActiveColor = TFT_WHITE;
const uint16_t StatusInactiveColor = TFT_DARKGREY;
}

HomeScreen::HomeScreen(Navigator &navigator,
                       TouchController &touch,
                       DisplayController &display,
                       SdCardController &sdCard,
                       WiFiController &wifi,
                       TimeController &time,
                       BatteryController &battery)
  : Screen(navigator, touch, display),
    _sdCard(sdCard),
    _wifi(wifi),
    _time(time),
    _battery(battery),
    _sdMounted(false),
    _wifiConnected(false),
    _batteryVoltage(0.0f),
    _batteryPercent(0),
    _lastTouchDown(false),
    _filesPressPending(false),
    _filesHoldTriggered(false),
    _filesPressedMs(0),
    _lastClockCheckMs(0xffffffffUL),
    _lastBatteryCheckMs(0),
    _lastClockText("") {
  _apps[0] = { "AI Assistant", AppAction::AiAssistant, 0xfe67, 0, 0, 0, 0 };
  _apps[1] = { "Browser", AppAction::Browser, 0xfb17, 0, 0, 0, 0 };
  _apps[2] = { "Files", AppAction::Files, 0x065f, 0, 0, 0, 0 };
  _apps[3] = { "Recorder", AppAction::Recorder, 0x9adf, 0, 0, 0, 0 };
  _apps[4] = { "Settings", AppAction::Settings, 0xfda0, 0, 0, 0, 0 };
  _apps[5] = { "USB Drive", AppAction::UsbDrive, 0x0799, 0, 0, 0, 0 };
  _apps[6] = { "Workout", AppAction::Workout, 0x04df, 0, 0, 0, 0 };
  _apps[7] = { "Apps", AppAction::Apps, 0x6318, 0, 0, 0, 0 };
}

void HomeScreen::onEnter() {
  refreshSdState();
  _wifiConnected = _wifi.isConnected();
  const BatteryReading batteryReading = _battery.read();
  _batteryVoltage = batteryReading.voltage;
  _batteryPercent = batteryReading.percentage;
  _lastBatteryCheckMs = millis();
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
    } else if (app.action == AppAction::AiAssistant) {
      _navigator.push(ScreenId::Assistant);
    } else if (app.action == AppAction::Recorder) {
      _navigator.push(ScreenId::VoiceRecorder);
    } else if (app.action == AppAction::Settings) {
      _navigator.push(ScreenId::Settings);
    } else if (app.action == AppAction::Browser) {
      _navigator.push(ScreenId::Browser);
    } else if (app.action == AppAction::UsbDrive) {
      _navigator.push(ScreenId::UsbDrive);
    } else if (app.action == AppAction::Workout) {
      _navigator.push(ScreenId::Workout);
    } else if (app.action == AppAction::Apps) {
      _navigator.push(ScreenId::Apps);
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
  const AppTile &files = _apps[FilesAppIndex];
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

  if (nowMs - _lastBatteryCheckMs >= BatteryRefreshIntervalMs) {
    setBatteryReading(_battery.read());
    _lastBatteryCheckMs = nowMs;
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
  TFT_eSPI &tft = _display.tft();
  const int16_t x = _display.width() - 46;
  const int16_t y = 4;
  const uint16_t fill = batteryFillColor(_batteryPercent);
  tft.fillRect(x, y, 46, 16, TFT_BLACK);
  tft.drawRect(x, y, 40, 16, TFT_WHITE);
  tft.fillRect(x + 40, y + 5, 3, 6, TFT_WHITE);
  tft.fillRect(x + 1, y + 1, 38, 14, fill);
  UiText::draw(tft, String(_batteryPercent), x + 20, y + 8, TFT_WHITE, fill,
               UiText::Size::Compact, MC_DATUM);
}

void HomeScreen::drawApps() {
  TFT_eSPI &tft = _display.tft();
  for (uint8_t i = 0; i < AppCount; i++) {
    const AppTile &app = _apps[i];
    int16_t centerX = app.x + app.w / 2;
    int16_t centerY = app.y + 48;
    tft.fillSmoothCircle(centerX, centerY, IconDiameter / 2,
                         app.circleColor, TFT_BLACK);
    drawAppIcon(app, centerX, centerY);

    String label = truncateLabel(String(app.label), app.w - 12);
    UiText::draw(tft, label, centerX, app.y + 94, TFT_WHITE, TFT_BLACK,
                 UiText::Size::Compact, TC_DATUM);
  }
}

void HomeScreen::drawAppIcon(const AppTile &app, int16_t centerX, int16_t centerY) {
  if (app.action == AppAction::Apps) {
    UiKit::drawAppsIcon(_display.tft(), centerX, centerY, TFT_WHITE, app.circleColor);
    return;
  }
  const uint16_t *colors = nullptr;
  const uint8_t *alpha = nullptr;
  uint16_t width = 0;
  uint16_t height = 0;

  if (app.action == AppAction::AiAssistant) {
    colors = ic_ai_colors;
    alpha = ic_ai_alpha;
    width = ic_ai_width;
    height = ic_ai_height;
  } else if (app.action == AppAction::Recorder) {
    colors = ic_recorder_colors;
    alpha = ic_recorder_alpha;
    width = ic_recorder_width;
    height = ic_recorder_height;
  } else if (app.action == AppAction::Settings) {
    colors = ic_settings_colors;
    alpha = ic_settings_alpha;
    width = ic_settings_width;
    height = ic_settings_height;
  } else if (app.action == AppAction::Workout) {
    colors = ic_workout_colors;
    alpha = ic_workout_alpha;
    width = ic_workout_width;
    height = ic_workout_height;
  } else if (app.action == AppAction::Files) {
    colors = ic_files_colors;
    alpha = ic_files_alpha;
    width = ic_files_width;
    height = ic_files_height;
  } else if (app.action == AppAction::Browser) {
    colors = ic_browser_colors;
    alpha = ic_browser_alpha;
    width = ic_browser_width;
    height = ic_browser_height;
  } else if (app.action == AppAction::UsbDrive) {
    colors = ic_usb_colors;
    alpha = ic_usb_alpha;
    width = ic_usb_width;
    height = ic_usb_height;
  }

  if (colors && alpha) {
    UiKit::drawRgba565Icon(_display.tft(), centerX - width / 2, centerY - height / 2,
                           width, height, colors, alpha, app.circleColor);
  }
}

String HomeScreen::clockText(const struct tm &localTime) const {
  char buffer[6];
  snprintf(buffer, sizeof(buffer), "%02d:%02d", localTime.tm_hour, localTime.tm_min);
  return String(buffer);
}

uint16_t HomeScreen::batteryFillColor(uint8_t percentage) const {
  return percentage >= 66 ? COLOR_BATTERY_GREEN :
         (percentage >= 33 ? COLOR_BATTERY_YELLOW : COLOR_BATTERY_RED);
}

String HomeScreen::batteryPercentageText(uint8_t percentage) const {
  return String(percentage);
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

void HomeScreen::setBatteryReading(const BatteryReading &reading) {
  const String previousPercentageText = batteryPercentageText(_batteryPercent);
  const uint16_t previousFill = batteryFillColor(_batteryPercent);
  const String nextPercentageText = batteryPercentageText(reading.percentage);
  const uint16_t nextFill = batteryFillColor(reading.percentage);
  if (previousPercentageText == nextPercentageText && previousFill == nextFill) {
    return;
  }
  _batteryVoltage = reading.voltage;
  _batteryPercent = reading.percentage;
  drawBattery();
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
