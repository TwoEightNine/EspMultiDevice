#pragma once

#include <Arduino.h>

class BatteryController {
public:
  explicit BatteryController(int adcPin);

  void begin();

  // Returns the current three-sample median battery-cell voltage after
  // voltage-divider correction.
  float voltage();
  uint8_t percentage();

private:
  static const uint8_t SampleCount = 3;

  int _adcPin;
  bool _hasReportedPercentage;
  uint8_t _reportedPercentage;

  float readMedianVoltage();
  uint8_t percentageForVoltage(float voltage) const;
};
