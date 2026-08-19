#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

class DisplayController {
public:
  static const int NoBacklightPin = -1;

  bool begin(uint8_t rotation = 1,
             int backlightPin = NoBacklightPin,
             bool backlightActiveHigh = true);

  bool isReady() const;
  TFT_eSPI &tft();

  uint16_t width();
  uint16_t height();

  void clear(uint16_t color = TFT_BLACK);

private:
  TFT_eSPI _tft;
  bool _ready = false;

  void configureBacklight(int backlightPin, bool backlightActiveHigh);
};
