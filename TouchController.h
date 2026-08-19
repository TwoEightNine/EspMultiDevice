#pragma once

#include <Arduino.h>
#include <SPI.h>

struct TouchPoint {
  bool touched = false;
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t z = 0;
  uint16_t rawX = 0;
  uint16_t rawY = 0;
};

class TouchController {
public:
  static const int NoIrqPin = -1;

  TouchController(int sckPin,
                  int misoPin,
                  int mosiPin,
                  int csPin,
                  int irqPin = NoIrqPin);

  bool begin(uint16_t screenWidth, uint16_t screenHeight, uint8_t rotation = 1);
  bool update();

  bool isTouched() const;
  TouchPoint point() const;

  void setRotation(uint8_t rotation);
  void setScreenSize(uint16_t screenWidth, uint16_t screenHeight);
  void setCalibration(uint16_t rawMinX, uint16_t rawMaxX, uint16_t rawMinY, uint16_t rawMaxY);

private:
  static const uint32_t SpiFrequency = 2500000;
  static const uint16_t TouchThreshold = 350;
  static const uint16_t PhantomCornerMargin = 90;
  static const uint8_t SamplesPerAxis = 3;

  int _sckPin;
  int _misoPin;
  int _mosiPin;
  int _csPin;
  int _irqPin;
  uint16_t _screenWidth;
  uint16_t _screenHeight;
  uint8_t _rotation;
  uint16_t _rawMinX;
  uint16_t _rawMaxX;
  uint16_t _rawMinY;
  uint16_t _rawMaxY;
  TouchPoint _point;
  SPISettings _spiSettings;

  void configurePins();
  bool readPoint(TouchPoint &point);
  uint16_t readRawCommand(uint8_t command);
  uint16_t readMedian(uint8_t command);
  uint16_t mapAxis(uint16_t value, uint16_t rawMin, uint16_t rawMax, uint16_t outMax) const;
  uint16_t mapInvertedAxis(uint16_t value, uint16_t rawMin, uint16_t rawMax, uint16_t outMax) const;
  bool isPhantomCorner(uint16_t rawX, uint16_t rawY) const;
  void mapToScreen(TouchPoint &point) const;
};
