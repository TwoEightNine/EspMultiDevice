#pragma once

#include <Arduino.h>

struct BatteryReading {
  float rawVoltage;
  float smoothVoltage;
};

// Stateful voltage smoothing kept separate from ADC access so its behavior can
// be exercised deterministically. A held increase still enters the moving
// window, allowing a sustained change to eventually clear the rise threshold.
class BatteryVoltageSmoother {
public:
  static const uint8_t WindowSamples = 30;
  static constexpr float MinimumAcceptedRise = 0.15f;
  static constexpr float HighVoltageRiseThreshold = 3.9f;

  BatteryVoltageSmoother();

  void reset();
  float add(float rawVoltage);

private:
  float _samples[WindowSamples];
  uint8_t _start;
  uint8_t _count;
  float _total;
  bool _hasAccepted;
  float _accepted;
};

class BatteryController {
public:
  explicit BatteryController(int adcPin);

  void begin();

  // Takes one three-sample median measurement and returns both that raw cell
  // voltage and the controller's accepted 30-sample moving-average voltage.
  BatteryReading read();

private:
  static const uint8_t SampleCount = 3;

  int _adcPin;
  BatteryVoltageSmoother _smoother;

  float readMedianVoltage();
};
