#include "BatteryController.h"

namespace {
const float DividerMultiplier = 2.0f;
// Float arithmetic can represent an exact decimal 0.15 V rise a few
// millionths low. This tolerance preserves the documented inclusive boundary
// without admitting a meaningfully smaller ADC change.
const float RiseComparisonEpsilon = 0.00001f;

}  // namespace

BatteryVoltageSmoother::BatteryVoltageSmoother() {
  reset();
}

void BatteryVoltageSmoother::reset() {
  for (uint8_t index = 0; index < WindowSamples; ++index) _samples[index] = 0.0f;
  _start = 0;
  _count = 0;
  _total = 0.0f;
  _hasAccepted = false;
  _accepted = 0.0f;
}

float BatteryVoltageSmoother::add(float rawVoltage) {
  if (_count == WindowSamples) {
    _total -= _samples[_start];
  } else {
    ++_count;
  }
  _samples[_start] = rawVoltage;
  _total += rawVoltage;
  _start = (_start + 1) % WindowSamples;

  const float candidate = _total / _count;
  const float rise = candidate - _accepted;
  if (!_hasAccepted || candidate <= _accepted ||
      rawVoltage > HighVoltageRiseThreshold ||
      rise + RiseComparisonEpsilon >= MinimumAcceptedRise) {
    _accepted = candidate;
    _hasAccepted = true;
  }
  return _accepted;
}

BatteryController::BatteryController(int adcPin)
  : _adcPin(adcPin) {
}

void BatteryController::begin() {
  _smoother.reset();
  pinMode(_adcPin, INPUT);
  analogReadResolution(12);
  // The 100 kΩ / 100 kΩ divider reaches 2.1 V at a fully charged 4.2 V cell.
  analogSetPinAttenuation(_adcPin, ADC_11db);
}

float BatteryController::readMedianVoltage() {
  // The first conversion after selecting an ADC channel is commonly noisy.
  (void)analogReadMilliVolts(_adcPin);

  uint32_t millivolts[SampleCount];
  for (uint8_t i = 0; i < SampleCount; ++i) {
    millivolts[i] = analogReadMilliVolts(_adcPin);
  }

  for (uint8_t i = 1; i < SampleCount; ++i) {
    uint32_t value = millivolts[i];
    uint8_t position = i;
    while (position > 0 && millivolts[position - 1] > value) {
      millivolts[position] = millivolts[position - 1];
      --position;
    }
    millivolts[position] = value;
  }

  return millivolts[SampleCount / 2] * DividerMultiplier / 1000.0f;
}

BatteryReading BatteryController::read() {
  const float rawVoltage = readMedianVoltage();
  return {rawVoltage, _smoother.add(rawVoltage)};
}
