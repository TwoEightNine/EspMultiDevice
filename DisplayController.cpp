#include "DisplayController.h"

bool DisplayController::begin(uint8_t rotation, int backlightPin, bool backlightActiveHigh) {
  configureBacklight(backlightPin, backlightActiveHigh);
  _tft.init();
  _tft.setRotation(rotation);
  _tft.setSwapBytes(true);
  _ready = true;
  showBootScreen();
  return _ready;
}

bool DisplayController::isReady() const {
  return _ready;
}

TFT_eSPI &DisplayController::tft() {
  return _tft;
}

uint16_t DisplayController::width() {
  return _tft.width();
}

uint16_t DisplayController::height() {
  return _tft.height();
}

void DisplayController::clear(uint16_t color) {
  if (!_ready) {
    return;
  }

  _tft.fillScreen(color);
}

void DisplayController::showBootScreen() {
  if (!_ready) {
    return;
  }

  const int16_t bandHeight = height() / 3;
  _tft.fillRect(0, 0, width(), bandHeight, TFT_RED);
  _tft.fillRect(0, bandHeight, width(), bandHeight, TFT_GREEN);
  _tft.fillRect(0, bandHeight * 2, width(), height() - bandHeight * 2, TFT_BLUE);
  delay(450);
  showStatus(F("ESP Media"), F("Display ready"), TFT_GREEN);
}

void DisplayController::showStatus(const String &title, const String &detail, uint16_t accentColor) {
  if (!_ready) {
    return;
  }

  clear(TFT_BLACK);
  drawFrame(accentColor);

  _tft.setTextDatum(MC_DATUM);
  _tft.setTextColor(TFT_WHITE, TFT_BLACK);
  _tft.drawString(title, width() / 2, height() / 2 - 16, 4);

  _tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  _tft.drawString(detail, width() / 2, height() / 2 + 20, 2);
  _tft.setTextDatum(TL_DATUM);
}

void DisplayController::showSdState(bool mounted, const String &detail) {
  showStatus(mounted ? F("SD card ready") : F("SD card offline"),
             detail,
             mounted ? TFT_GREEN : TFT_ORANGE);
}

void DisplayController::configureBacklight(int backlightPin, bool backlightActiveHigh) {
  if (backlightPin == NoBacklightPin) {
    return;
  }

  pinMode(backlightPin, OUTPUT);
  digitalWrite(backlightPin, backlightActiveHigh ? HIGH : LOW);
}

void DisplayController::drawFrame(uint16_t accentColor) {
  const int16_t inset = 8;
  _tft.drawRect(inset, inset, width() - inset * 2, height() - inset * 2, TFT_DARKGREY);
  _tft.fillRect(inset, inset, width() - inset * 2, 4, accentColor);
}
