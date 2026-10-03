#pragma once

#include <Arduino.h>
#include <SPI.h>

#include "DisplayOrientation.h"

struct TouchPoint {
  bool touched = false;
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t z = 0;
  uint16_t rawX = 0;
  uint16_t rawY = 0;
};

struct TouchCalibrationGrid {
  uint16_t rawX[4][4];
  uint16_t rawY[4][4];
  uint16_t rawZ[4][4];
};

class TouchController {
public:
  static const int NoIrqPin = -1;

  TouchController(int sckPin,
                  int misoPin,
                  int mosiPin,
                  int csPin,
                  int irqPin = NoIrqPin);

  bool begin(uint16_t screenWidth,
             uint16_t screenHeight,
             uint8_t rotation = DisplayOrientation::Portrait);
  bool update();

  bool isTouched() const;
  TouchPoint point() const;
  bool hasMeshCalibration() const;

  void setRotation(uint8_t rotation);
  void setScreenSize(uint16_t screenWidth, uint16_t screenHeight);
  void setCalibration(uint16_t rawMinX, uint16_t rawMaxX, uint16_t rawMinY, uint16_t rawMaxY);
  // Samples are ordered in the current logical display orientation. They are
  // normalized to the panel-native rotation-0 order before use or persistence.
  bool setCalibration(const TouchCalibrationGrid &grid, bool persist = true);
  bool loadCalibration();
  void setCalibrationCapture(bool enabled);

private:
  static const uint32_t SpiFrequency = 2500000;
  static const uint32_t PressDebounceMs = 25;
  static const uint32_t ReleaseDebounceMs = 40;
  static const uint8_t HoldPressurePercent = 80;
  static const uint16_t PhantomCornerMargin = 90;
  static const uint8_t SamplesPerAxis = 3;
  static const uint16_t CalibrationNoiseFloor = 350;

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
  bool _hasMeshCalibration;
  bool _calibrationCapture;
  bool _pendingTouched;
  uint32_t _pendingSinceMs;
  TouchCalibrationGrid _calibration;
  TouchPoint _point;
  SPISettings _spiSettings;

  void configurePins();
  bool readPoint(TouchPoint &point, bool holdingTouch);
  uint16_t readRawCommand(uint8_t command);
  uint16_t readFilteredAxis(uint8_t command);
  uint16_t mapAxis(uint16_t value, uint16_t rawMin, uint16_t rawMax, uint16_t outMax) const;
  uint16_t mapInvertedAxis(uint16_t value, uint16_t rawMin, uint16_t rawMax, uint16_t outMax) const;
  bool isPhantomCorner(uint16_t rawX, uint16_t rawY) const;
  void mapToScreen(TouchPoint &point) const;
  bool mapWithMesh(TouchPoint &point, uint16_t &localPressureThreshold) const;
  bool solveCell(float rawX,
                 float rawY,
                 uint8_t cellX,
                 uint8_t cellY,
                 float &s,
                 float &t,
                 float &error) const;
  bool validateGrid(const TouchCalibrationGrid &grid) const;
  bool saveCalibration() const;
};
