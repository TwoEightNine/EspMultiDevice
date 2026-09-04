#include "DisplayController.h"

bool DisplayController::begin(uint8_t rotation, int backlightPin, bool backlightActiveHigh) {
  configureBacklight(backlightPin, backlightActiveHigh);
  _tft.init();
  // Keep the always-lit panel blank while the first framebuffer is written.
  _tft.writecommand(TFT_DISPOFF);
  _tft.setRotation(rotation);
  _tft.setSwapBytes(true);
  _tft.fillScreen(TFT_BLACK);
  _tft.writecommand(TFT_DISPON);
  _ready = true;
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

void DisplayController::configureBacklight(int backlightPin, bool backlightActiveHigh) {
  if (backlightPin == NoBacklightPin) {
    return;
  }

  pinMode(backlightPin, OUTPUT);
  digitalWrite(backlightPin, backlightActiveHigh ? HIGH : LOW);
}
