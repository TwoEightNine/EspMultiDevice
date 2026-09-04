#include "BatteryController.h"

namespace {

struct ChargePoint {
  float voltage;
  uint8_t percentage;
};

const ChargePoint RestingChargeCurve[] = {
  {3.20f, 0},
  {3.50f, 5},
  {3.60f, 10},
  {3.70f, 20},
  {3.80f, 40},
  {3.90f, 60},
  {4.00f, 80},
  {4.10f, 90},
  {4.20f, 100},
};

const uint8_t ChargePointCount = sizeof(RestingChargeCurve) / sizeof(RestingChargeCurve[0]);
const float DividerMultiplier = 2.0f;

}  // namespace

BatteryController::BatteryController(int adcPin)
  : _adcPin(adcPin) {
}

void BatteryController::begin() {
  pinMode(_adcPin, INPUT);
  analogReadResolution(12);
  // The 100 kΩ / 100 kΩ divider reaches 2.1 V at a fully charged 4.2 V cell.
  analogSetPinAttenuation(_adcPin, ADC_11db);
}

float BatteryController::voltage() {
  // The first conversion after selecting an ADC channel is commonly noisy.
  (void)analogReadMilliVolts(_adcPin);

  uint32_t totalMillivolts = 0;
  for (uint8_t i = 0; i < SampleCount; ++i) {
    totalMillivolts += analogReadMilliVolts(_adcPin);
  }

  const float dividerMillivolts = (float)totalMillivolts / SampleCount;
  return dividerMillivolts * DividerMultiplier / 1000.0f;
}

uint8_t BatteryController::percentage() {
  return percentageForVoltage(voltage());
}

uint8_t BatteryController::percentageForVoltage(float measuredVoltage) const {
  if (measuredVoltage <= RestingChargeCurve[0].voltage) {
    return RestingChargeCurve[0].percentage;
  }

  for (uint8_t i = 1; i < ChargePointCount; ++i) {
    const ChargePoint &lower = RestingChargeCurve[i - 1];
    const ChargePoint &upper = RestingChargeCurve[i];
    if (measuredVoltage <= upper.voltage) {
      const float position = (measuredVoltage - lower.voltage) / (upper.voltage - lower.voltage);
      return (uint8_t)(lower.percentage + position * (upper.percentage - lower.percentage) + 0.5f);
    }
  }

  return RestingChargeCurve[ChargePointCount - 1].percentage;
}
