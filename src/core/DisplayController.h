#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

#include "DisplayDmaBlitter.h"
#include "DisplayOrientation.h"

class DisplayController {
public:
  static const int NoBacklightPin = -1;

  bool begin(uint8_t rotation = DisplayOrientation::Portrait,
             int backlightPin = NoBacklightPin,
             bool backlightActiveHigh = true);

  bool isReady() const;
  // Waits for any DMA image transfer first, so TFT_eSPI always owns its pins.
  TFT_eSPI &tft();

  // Starts an asynchronous LCD_CAM DMA transfer of a native RGB565 rectangle.
  // The 64-byte-aligned buffer must stay unchanged until dmaImageActive() is
  // false. Returns false when DMA is unavailable; the caller then uses tft().
  bool beginDmaImage(int16_t x, int16_t y, uint16_t width, uint16_t height, const uint16_t *pixels);
  bool dmaImageActive() const;
  void finishDmaImage();
  // Releases the LCD_CAM bus and its DMA descriptors.
  void endDmaImages();

  // Runtime presentation changes (such as video landscape mode) must update
  // the paired TouchController with the same TFT_eSPI rotation.
  void setRotation(uint8_t rotation);
  uint16_t width();
  uint16_t height();

  void clear(uint16_t color = TFT_BLACK);

private:
  TFT_eSPI _tft;
  DisplayDmaBlitter _dma;
  bool _ready = false;

  void configureBacklight(int backlightPin, bool backlightActiveHigh);
};
