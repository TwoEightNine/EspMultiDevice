#pragma once

#include <Arduino.h>

#include "../../core/BatteryController.h"

class SdCardController;

struct BatteryTelemetryReading {
  BatteryReading raw;
  BatteryReading smooth;
  uint32_t elapsedMs;
};

// A bounded, time-weighted summary point used only for drawing the complete
// current-boot chart. The full one-second data remains in the SD log.
struct BatteryHistoryPoint {
  uint32_t firstElapsedMs;
  uint32_t lastElapsedMs;
  uint32_t sampleCount;
  float smoothVoltageTotal;
  float smoothPercentageTotal;
};

class BatteryHistoryService {
public:
  static const uint16_t MaxChartPoints = 256;

  BatteryHistoryService(BatteryController &battery, SdCardController &sdCard);

  // Starts a new in-memory and on-card boot session and records its first
  // live sample. Calling code must have initialized BatteryController first.
  void begin(uint32_t nowMs);
  // Samples once per elapsed second without blocking the UI loop.
  void update(uint32_t nowMs);

  const BatteryTelemetryReading &latest() const;
  uint16_t historyCount() const;
  const BatteryHistoryPoint &historyPoint(uint16_t index) const;
  uint32_t historyDurationMs() const;
  uint32_t generation() const;

private:
  static const uint8_t SmoothingWindowSamples = 30;
  static const uint32_t SampleIntervalMs = 1000UL;

  BatteryController &_battery;
  SdCardController &_sdCard;
  BatteryTelemetryReading _latest;
  float _smoothingSamples[SmoothingWindowSamples];
  uint8_t _smoothingStart;
  uint8_t _smoothingCount;
  float _smoothingTotal;
  BatteryHistoryPoint _history[MaxChartPoints];
  uint16_t _historyCount;
  uint32_t _bootStartedMs;
  uint32_t _lastSampleMs;
  uint32_t _generation;
  bool _started;
  bool _logReady;
  bool _cardWasMounted;

  void sample(uint32_t nowMs);
  void appendHistory(const BatteryTelemetryReading &reading);
  void compactHistory();
  bool ensureLog();
  bool validateCurrentLog();
  bool createLog();
  bool appendLogRecord(const BatteryTelemetryReading &reading);
};
