#pragma once

#include <Arduino.h>

struct BatteryReading {
  float voltage;
  uint8_t percentage;
};

class BatteryController {
public:
  explicit BatteryController(int adcPin);

  void begin();

  // Takes one three-sample median measurement. The returned value is retained
  // at the lowest median voltage seen during this controller session, and its
  // percentage is derived from that same returned voltage.
  BatteryReading read();

private:
  static const uint8_t SampleCount = 3;

  int _adcPin;
  bool _hasReturnedVoltage;
  float _returnedVoltage;

  float readMedianVoltage();
  uint8_t percentageForVoltage(float voltage) const;
};
