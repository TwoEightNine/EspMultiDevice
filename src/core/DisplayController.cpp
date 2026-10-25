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
  finishDmaImage();
  return _tft;
}

bool DisplayController::beginDmaImage(int16_t x, int16_t y, uint16_t width, uint16_t height,
                                      const uint16_t *pixels) {
  if (!_ready) {
    return false;
  }
  finishDmaImage();
  const uint32_t maxBytes = (uint32_t)_tft.width() * _tft.height() * sizeof(uint16_t);
  return _dma.start(x, y, width, height, pixels, maxBytes);
}

bool DisplayController::dmaImageActive() const {
  return _dma.busy();
}

void DisplayController::finishDmaImage() {
  if (!_dma.finish()) {
    return;
  }
  // DMA changed the panel address window behind TFT_eSPI's drawPixel cache.
  _tft.startWrite();
  _tft.setWindow(0, 0, 0, 0);
  _tft.endWrite();
}

void DisplayController::endDmaImages() {
  finishDmaImage();
  _dma.end();
}

void DisplayController::setRotation(uint8_t rotation) {
  if (!_ready) {
    return;
  }
  finishDmaImage();
  _tft.setRotation(rotation % 4);
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

  finishDmaImage();
  _tft.fillScreen(color);
}

void DisplayController::configureBacklight(int backlightPin, bool backlightActiveHigh) {
  if (backlightPin == NoBacklightPin) {
    return;
  }

  pinMode(backlightPin, OUTPUT);
  digitalWrite(backlightPin, backlightActiveHigh ? HIGH : LOW);
}
