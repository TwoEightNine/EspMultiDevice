#include "BatteryHistoryService.h"

#include <FS.h>
#include <SD.h>

#include "../../core/SpiBusLock.h"
#include "SdCardController.h"

namespace {

constexpr const char *SystemDirectory = "/.system";
constexpr const char *HistoryPath = "/.system/battery_history.bin";
constexpr const char *HistoryTempPath = "/.system/battery_history.bin.tmp";
constexpr uint32_t HistoryMagic = 0x42544831UL;  // BTH1
constexpr uint16_t HistoryVersion = 2;
const TickType_t StorageLockTimeout = pdMS_TO_TICKS(20);

struct __attribute__((packed)) HistoryHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t headerBytes;
  uint16_t recordBytes;
  uint16_t sampleIntervalMs;
  uint32_t bootStartedMs;
};

struct __attribute__((packed)) HistoryRecord {
  uint32_t elapsedMs;
  uint16_t rawMillivolts;
  uint16_t smoothMillivolts;
  uint16_t checksum;
};

static_assert(sizeof(HistoryRecord) == 10, "Battery history v2 record size changed");

uint16_t millivoltsFor(float voltage) {
  if (voltage <= 0.0f) return 0;
  const float millivolts = voltage * 1000.0f + 0.5f;
  return millivolts >= 65535.0f ? 65535 : static_cast<uint16_t>(millivolts);
}

uint16_t recordChecksum(const HistoryRecord &record) {
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&record);
  uint16_t checksum = 0x4d3b;
  for (size_t index = 0; index < sizeof(HistoryRecord) - sizeof(record.checksum); ++index) {
    checksum = static_cast<uint16_t>((checksum << 5) | (checksum >> 11));
    checksum ^= bytes[index];
  }
  return checksum;
}

}  // namespace

BatteryHistoryService::BatteryHistoryService(BatteryController &battery,
                                             SdCardController &sdCard)
  : _battery(battery),
    _sdCard(sdCard),
    _latest{0.0f, 0.0f, 0},
    _history{},
    _historyCount(0),
    _bootStartedMs(0),
    _lastSampleMs(0),
    _generation(0),
    _started(false),
    _logReady(false),
    _cardWasMounted(false) {
}

void BatteryHistoryService::begin(uint32_t nowMs) {
  _historyCount = 0;
  _bootStartedMs = nowMs;
  _lastSampleMs = nowMs;
  _generation = 0;
  _started = true;
  _logReady = false;
  _cardWasMounted = false;
  sample(nowMs);
}

void BatteryHistoryService::update(uint32_t nowMs) {
  if (!_started) {
    begin(nowMs);
    return;
  }
  if (nowMs - _lastSampleMs < SampleIntervalMs) return;

  _lastSampleMs = nowMs;
  sample(nowMs);
}

const BatteryTelemetryReading &BatteryHistoryService::latest() const {
  return _latest;
}

uint16_t BatteryHistoryService::historyCount() const {
  return _historyCount;
}

const BatteryHistoryPoint &BatteryHistoryService::historyPoint(uint16_t index) const {
  return _history[index < _historyCount ? index : 0];
}

uint32_t BatteryHistoryService::historyDurationMs() const {
  return _latest.elapsedMs;
}

uint32_t BatteryHistoryService::generation() const {
  return _generation;
}

void BatteryHistoryService::sample(uint32_t nowMs) {
  const BatteryReading reading = _battery.read();
  _latest = {reading.rawVoltage, reading.smoothVoltage, nowMs - _bootStartedMs};
  appendHistory(_latest);
  _generation++;
  if (ensureLog()) {
    appendLogRecord(_latest);
  }
}

void BatteryHistoryService::appendHistory(const BatteryTelemetryReading &reading) {
  if (_historyCount == MaxChartPoints) compactHistory();

  BatteryHistoryPoint &point = _history[_historyCount++];
  point.firstElapsedMs = reading.elapsedMs;
  point.lastElapsedMs = reading.elapsedMs;
  point.sampleCount = 1;
  point.smoothVoltageTotal = reading.smoothVoltage;
}

void BatteryHistoryService::compactHistory() {
  uint16_t output = 0;
  for (uint16_t input = 0; input < _historyCount; input += 2) {
    const BatteryHistoryPoint &first = _history[input];
    if (input + 1 == _historyCount) {
      _history[output++] = first;
      continue;
    }

    const BatteryHistoryPoint &second = _history[input + 1];
    BatteryHistoryPoint &merged = _history[output++];
    merged.firstElapsedMs = first.firstElapsedMs;
    merged.lastElapsedMs = second.lastElapsedMs;
    merged.sampleCount = first.sampleCount + second.sampleCount;
    merged.smoothVoltageTotal = first.smoothVoltageTotal + second.smoothVoltageTotal;
  }
  _historyCount = output;
}

bool BatteryHistoryService::ensureLog() {
  if (!_sdCard.isMounted()) {
    _cardWasMounted = false;
    return false;
  }

  if (!_cardWasMounted) {
    _cardWasMounted = true;
    if (_logReady && validateCurrentLog()) return true;
    _logReady = false;
  }
  if (_logReady) return true;
  return createLog();
}

bool BatteryHistoryService::validateCurrentLog() {
  if (!_sdCard.isMounted() || !SpiBusLock::lock(StorageLockTimeout)) return false;

  File file = SD.open(HistoryPath, FILE_READ);
  HistoryHeader header{};
  const bool readHeader = file &&
                          file.read(reinterpret_cast<uint8_t *>(&header), sizeof(header)) == sizeof(header);
  const size_t fileSize = file ? file.size() : 0;
  if (file) file.close();
  SpiBusLock::unlock();

  if (!readHeader || header.magic != HistoryMagic || header.version != HistoryVersion ||
      header.headerBytes != sizeof(HistoryHeader) || header.recordBytes != sizeof(HistoryRecord) ||
      header.sampleIntervalMs != SampleIntervalMs || header.bootStartedMs != _bootStartedMs ||
      fileSize < sizeof(HistoryHeader)) {
    return false;
  }

  // A reader consumes only complete records; a card removal during the final
  // write can therefore leave at most one ignored trailing partial record.
  return true;
}

bool BatteryHistoryService::createLog() {
  if (!_sdCard.isMounted() || !SpiBusLock::lock(StorageLockTimeout)) return false;

  bool success = SD.exists(SystemDirectory) || SD.mkdir(SystemDirectory);
  if (success && SD.exists(HistoryTempPath)) {
    SD.remove(HistoryTempPath);
  }
  if (success) {
    File file = SD.open(HistoryTempPath, FILE_WRITE);
    if (!file) {
      success = false;
    } else {
      const HistoryHeader header = {
        HistoryMagic,
        HistoryVersion,
        sizeof(HistoryHeader),
        sizeof(HistoryRecord),
        SampleIntervalMs,
        _bootStartedMs,
      };
      success = file.write(reinterpret_cast<const uint8_t *>(&header), sizeof(header)) == sizeof(header);
      file.close();
    }
  }
  if (success && SD.exists(HistoryPath)) {
    success = SD.remove(HistoryPath);
  }
  if (success) {
    success = SD.rename(HistoryTempPath, HistoryPath);
  }
  if (!success && SD.exists(HistoryTempPath)) {
    SD.remove(HistoryTempPath);
  }
  SpiBusLock::unlock();
  _logReady = success;
  return success;
}

bool BatteryHistoryService::appendLogRecord(const BatteryTelemetryReading &reading) {
  if (!_sdCard.isMounted() || !SpiBusLock::lock(StorageLockTimeout)) return false;

  HistoryRecord record = {
    reading.elapsedMs,
    millivoltsFor(reading.rawVoltage),
    millivoltsFor(reading.smoothVoltage),
    0,
  };
  record.checksum = recordChecksum(record);
  File file = SD.open(HistoryPath, FILE_APPEND);
  const bool success = file &&
                       file.write(reinterpret_cast<const uint8_t *>(&record), sizeof(record)) == sizeof(record);
  if (file) file.close();
  SpiBusLock::unlock();
  if (!success) _logReady = false;
  return success;
}
