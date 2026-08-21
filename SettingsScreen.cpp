#include "SettingsScreen.h"

#include "Navigator.h"

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

  if (point.x >= 10 && point.x < 90 && point.y >= 10 && point.y < 44) {
    _navigator.pop();
    return;
  }

  if (point.x >= ListPadding && point.x < _display.width() - ListPadding &&
      point.y >= RowTop && point.y < RowTop + OptionCount * RowHeight) {
    uint8_t option = (point.y - RowTop) / RowHeight;
    if (option == 0) {
      _navigator.push(ScreenId::WiFi);
    } else if (option == 1) {
      _navigator.push(ScreenId::TouchCalibration);
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
  TFT_eSPI &tft = _display.tft();
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawLine(18, 27, 34, 19, TFT_WHITE);
  tft.drawLine(18, 27, 34, 35, TFT_WHITE);
  tft.drawLine(18, 27, 48, 27, TFT_WHITE);
  tft.drawString(F("Settings"), 108, 16, 4);
}

void SettingsScreen::drawOptions() {
  TFT_eSPI &tft = _display.tft();
  const char *labels[OptionCount] = { "Wi-Fi", "Touch screen", "About" };
  const int16_t listWidth = _display.width() - 2 * ListPadding;

  for (uint8_t i = 0; i < OptionCount; i++) {
    int16_t y = RowTop + i * RowHeight;
    tft.fillRect(ListPadding, y, listWidth, RowHeight, TFT_BLACK);

    tft.fillRect(ListPadding + 2, y + 7, 16, 16, TFT_WHITE);
    // Replace the rectangle with the final option bitmap when available:
    // tft.pushImage(ListPadding + 2, y + 7, 16, 16, settingsOptionBitmap);

    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(labels[i], ListPadding + 28, y + RowHeight / 2, 2);
    tft.setTextDatum(TL_DATUM);
  }
}
