#include "BatteryController.h"

namespace {

struct ChargePoint {
  float voltage;
  uint8_t percentage;
};

// One point per 30-minute observation from the measured discharge session.
// Percentages progress uniformly through that session: 4.00 V is full and
// 3.30 V is empty. Values between points use linear interpolation.
const ChargePoint DischargeCurve[] = {
  {3.30f, 0},
  {3.35f, 9},
  {3.45f, 18},
  {3.55f, 27},
  {3.60f, 36},
  {3.64f, 45},
  {3.68f, 55},
  {3.7025f, 64},
  {3.75f, 73},
  {3.81f, 82},
  {3.90f, 91},
  {4.00f, 100},
};

const uint8_t ChargePointCount = sizeof(DischargeCurve) / sizeof(DischargeCurve[0]);
const float DividerMultiplier = 2.0f;

}  // namespace

BatteryController::BatteryController(int adcPin)
  : _adcPin(adcPin),
    _hasReturnedVoltage(false),
    _returnedVoltage(0.0f) {
}

void BatteryController::begin() {
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
  const float medianVoltage = readMedianVoltage();
  if (!_hasReturnedVoltage || medianVoltage < _returnedVoltage) {
    _returnedVoltage = medianVoltage;
    _hasReturnedVoltage = true;
  }

  // Derive percentage from precisely the voltage returned to the caller.
  // The strictly ascending curve and retained voltage keep both fields from
  // increasing for the lifetime of this controller instance.
  return {_returnedVoltage, percentageForVoltage(_returnedVoltage)};
}

BatteryReading BatteryController::readLive() {
  const float medianVoltage = readMedianVoltage();
  return readingForVoltage(medianVoltage);
}

BatteryReading BatteryController::readingForVoltage(float voltage) const {
  return {voltage, percentageForVoltage(voltage)};
}

uint8_t BatteryController::percentageForVoltage(float measuredVoltage) const {
  if (measuredVoltage <= DischargeCurve[0].voltage) {
    return DischargeCurve[0].percentage;
  }

  for (uint8_t i = 1; i < ChargePointCount; ++i) {
    const ChargePoint &lower = DischargeCurve[i - 1];
    const ChargePoint &upper = DischargeCurve[i];
    if (measuredVoltage <= upper.voltage) {
      const float position = (measuredVoltage - lower.voltage) / (upper.voltage - lower.voltage);
      return (uint8_t)(lower.percentage + position * (upper.percentage - lower.percentage) + 0.5f);
    }
  }

  return DischargeCurve[ChargePointCount - 1].percentage;
}
