#include "SettingsScreen.h"

#include "../../app/Navigator.h"
#include "../../ui/UiKit.h"

SettingsScreen::SettingsScreen(Navigator &navigator,
                               TouchController &touch,
                               DisplayController &display)
  : Screen(navigator, touch, display),
    _lastTouchDown(false) {
}

void SettingsScreen::onEnter() {
  _lastTouchDown = false;
  draw();
}

void SettingsScreen::handleTouch(const TouchPoint &point) {
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

  if (point.x >= 0 && point.x < _display.width() &&
      point.y >= RowTop && point.y < RowTop + OptionCount * RowHeight) {
    uint8_t option = (point.y - RowTop) / RowHeight;
    if (option == 0) {
      _navigator.push(ScreenId::WiFi);
    } else if (option == 1) {
      _navigator.push(ScreenId::WiFiSpeedTest);
    } else if (option == 2) {
      _navigator.push(ScreenId::AudioSettings);
    } else if (option == 3) {
      _navigator.push(ScreenId::Led);
    } else if (option == 4) {
      _navigator.push(ScreenId::BatteryInfo);
    } else if (option == 5) {
      _navigator.push(ScreenId::TouchCalibration);
    } else if (option == 6) {
      _navigator.push(ScreenId::About);
    }
    return;
  }
}

void SettingsScreen::tick(uint32_t nowMs) {
  (void)nowMs;
}

void SettingsScreen::draw() {
  if (!_display.isReady()) {
    return;
  }

  _display.tft().fillScreen(TFT_BLACK);
  drawTopBar();
  drawOptions();
}

void SettingsScreen::drawTopBar() {
  UiKit::drawTopAppBar(_display.tft(), _display.width(), F("Settings"));
}

void SettingsScreen::drawOptions() {
  const char *labels[OptionCount] = {
    "Wi-Fi", "Speed test", "Audio", "LED", "Battery", "Touch screen", "About"
  };

  for (uint8_t i = 0; i < OptionCount; i++) {
    int16_t y = RowTop + i * RowHeight;
    UiKit::drawListCell(_display.tft(), _display.width(), y, labels[i]);
  }
}
