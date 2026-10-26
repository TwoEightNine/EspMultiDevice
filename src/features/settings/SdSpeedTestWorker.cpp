#include "SdSpeedTestWorker.h"

namespace {
class StateLock {
public:
  explicit StateLock(SemaphoreHandle_t mutex)
      : _mutex(mutex),
        _locked(mutex && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {}
  ~StateLock() { if (_locked) xSemaphoreGive(_mutex); }
  bool locked() const { return _locked; }

private:
  SemaphoreHandle_t _mutex;
  bool _locked;
};

float mebibytesPerSecond(uint32_t bytes, uint32_t elapsedMs) {
  if (elapsedMs == 0) elapsedMs = 1;
  return (static_cast<float>(bytes) * 1000.0f) /
         (1024.0f * 1024.0f * static_cast<float>(elapsedMs));
}
}  // namespace

SdSpeedTestWorker::SdSpeedTestWorker(SdCardController &sdCard)
    : _sdCard(sdCard),
      _snapshotMutex(nullptr),
      _task(nullptr),
      _generation(1),
      _taskGeneration(0),
      _active(false),
      _snapshot{},
      _lastProgressPublishMs(0),
      _lastBenchmarkPhase(SdThroughputBenchmarkPhase::Write) {}

bool SdSpeedTestWorker::begin() {
  if (!_snapshotMutex) _snapshotMutex = xSemaphoreCreateMutex();
  if (!_snapshotMutex) return false;
  StateLock lock(_snapshotMutex);
  if (!lock.locked()) return false;
  const uint32_t revision = _snapshot.revision + 1;
  _snapshot = {};
  _snapshot.revision = revision;
  strlcpy(_snapshot.status, "Ready", sizeof(_snapshot.status));
  return true;
}

bool SdSpeedTestWorker::start() {
  if (_active || !_sdCard.isMounted()) return false;
  if (!begin()) return false;
  _generation = _generation + 1;
  if (_generation == 0) _generation = 1;
  _taskGeneration = _generation;
  _active = true;
  _lastProgressPublishMs = 0;
  _lastBenchmarkPhase = SdThroughputBenchmarkPhase::Write;
  publish(SdSpeedTestPhase::Write, 0, "Preparing temporary file");
#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
  const BaseType_t workerCore = 0;
#else
  const BaseType_t workerCore = ARDUINO_RUNNING_CORE == 0 ? 1 : 0;
#endif
  if (xTaskCreatePinnedToCore(taskMain, "sd-speed", 6144, this,
                              tskIDLE_PRIORITY + 1, &_task,
                              workerCore) != pdPASS) {
    _active = false;
    _task = nullptr;
    publish(SdSpeedTestPhase::Error, 0, "SD speed-test worker unavailable");
    return false;
  }
  Serial.printf("[SD speed test] worker started on core %d\n",
                static_cast<int>(workerCore));
  return true;
}

void SdSpeedTestWorker::cancel() {
  if (!_active) return;
  _generation = _generation + 1;
  if (_generation == 0) _generation = 1;
  publish(SdSpeedTestPhase::Cancelled, 0, "Cancelling...");
}

bool SdSpeedTestWorker::end() {
  cancel();
  if (_active) return false;
  _task = nullptr;
  if (_snapshotMutex) {
    vSemaphoreDelete(_snapshotMutex);
    _snapshotMutex = nullptr;
  }
  return true;
}

SdSpeedTestSnapshot SdSpeedTestWorker::snapshot() const {
  if (!_snapshotMutex) return _snapshot;
  StateLock lock(_snapshotMutex);
  return lock.locked() ? _snapshot : SdSpeedTestSnapshot{};
}

void SdSpeedTestWorker::taskMain(void *context) {
  SdSpeedTestWorker *worker = static_cast<SdSpeedTestWorker *>(context);
  worker->run(worker->_taskGeneration);
  if (!worker->current(worker->_taskGeneration) &&
      worker->snapshot().phase != SdSpeedTestPhase::Cancelled) {
    worker->publish(SdSpeedTestPhase::Cancelled, 0, "Test cancelled");
  }
  worker->_task = nullptr;
  worker->_active = false;
  Serial.println(F("[SD speed test] worker stopped"));
  vTaskDelete(nullptr);
}

void SdSpeedTestWorker::run(uint32_t generation) {
  SdThroughputBenchmarkResult result;
  const bool succeeded = _sdCard.runThroughputBenchmark(result, this);
  if (!current(generation) || result.cancelled) {
    publish(SdSpeedTestPhase::Cancelled, 0,
            result.cleanupComplete ? "Test cancelled"
                                   : "Cancelled; cleanup incomplete");
    return;
  }
  if (!succeeded) {
    publish(SdSpeedTestPhase::Error, 0,
            result.error[0] ? result.error : "SD speed test failed");
    return;
  }
  char status[sizeof(SdSpeedTestSnapshot::status)];
  snprintf(status, sizeof(status), "Verified at %.2f MHz; temporary file removed",
           result.clockHz / 1000000.0f);
  publish(SdSpeedTestPhase::Complete, 100, status,
          mebibytesPerSecond(result.transferBytes, result.writeElapsedMs),
          mebibytesPerSecond(result.transferBytes, result.readElapsedMs),
          mebibytesPerSecond(result.transferBytes, result.bulkReadElapsedMs),
          result.verified);
}

bool SdSpeedTestWorker::current(uint32_t generation) const {
  return generation != 0 && generation == _generation;
}

void SdSpeedTestWorker::publish(SdSpeedTestPhase phase, uint8_t progress,
                                const char *status, float writeMiBps,
                                float readMiBps, float bulkReadMiBps,
                                bool verified) {
  if (!_snapshotMutex) return;
  StateLock lock(_snapshotMutex);
  if (!lock.locked()) return;
  _snapshot.phase = phase;
  _snapshot.progress = progress > 100 ? 100 : progress;
  _snapshot.writeMiBps = writeMiBps;
  _snapshot.readMiBps = readMiBps;
  _snapshot.bulkReadMiBps = bulkReadMiBps;
  _snapshot.verified = verified;
  strlcpy(_snapshot.status, status ? status : "", sizeof(_snapshot.status));
  _snapshot.revision++;
}

bool SdSpeedTestWorker::sdBenchmarkCancelled() const {
  return !current(_taskGeneration);
}

void SdSpeedTestWorker::sdBenchmarkProgress(
    SdThroughputBenchmarkPhase phase, uint32_t completedBytes,
    uint32_t totalBytes) {
  if (!current(_taskGeneration) || totalBytes == 0) return;
  const uint32_t now = millis();
  const bool phaseChanged = phase != _lastBenchmarkPhase;
  if (!phaseChanged && completedBytes < totalBytes &&
      now - _lastProgressPublishMs < 125) return;
  _lastBenchmarkPhase = phase;
  _lastProgressPublishMs = now;
  publish(phase == SdThroughputBenchmarkPhase::Write
              ? SdSpeedTestPhase::Write : SdSpeedTestPhase::Read,
          static_cast<uint8_t>((completedBytes * 100ULL) / totalBytes),
          phase == SdThroughputBenchmarkPhase::Write ? "Writing 4 MiB"
          : phase == SdThroughputBenchmarkPhase::Read
              ? "Reading and verifying 4 MiB"
              : "Re-reading 4 MiB in 32 KiB blocks");
}
