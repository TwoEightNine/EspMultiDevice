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

  // Takes one three-sample median measurement, applies voltage stabilization,
  // and returns the corresponding cell voltage and charge-curve percentage.
  BatteryReading read();

private:
  static const uint8_t SampleCount = 3;

  int _adcPin;
  bool _hasFilteredVoltage;
  float _filteredVoltage;

  float readMedianVoltage();
  uint8_t percentageForVoltage(float voltage) const;
};
