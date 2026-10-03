#pragma once

#include <Arduino.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "../../services/storage/SdCardController.h"

enum class SdSpeedTestPhase : uint8_t {
  Idle,
  Write,
  Read,
  Complete,
  Cancelled,
  Error
};

struct SdSpeedTestSnapshot {
  SdSpeedTestPhase phase = SdSpeedTestPhase::Idle;
  uint32_t revision = 0;
  uint8_t progress = 0;
  float writeMiBps = -1.0f;
  float readMiBps = -1.0f;
  bool verified = false;
  char status[80] = {};
};

// Screen-owned, one-shot SD worker. SdCardController performs the benchmark
// using short SPI transactions; this worker only publishes immutable UI
// snapshots and generation-safe cancellation.
class SdSpeedTestWorker : private SdThroughputBenchmarkObserver {
public:
  explicit SdSpeedTestWorker(SdCardController &sdCard);

  bool begin();
  bool start();
  void cancel();
  bool end();
  bool active() const { return _active; }
  SdSpeedTestSnapshot snapshot() const;

private:
  SdCardController &_sdCard;
  mutable SemaphoreHandle_t _snapshotMutex;
  TaskHandle_t _task;
  volatile uint32_t _generation;
  volatile uint32_t _taskGeneration;
  volatile bool _active;
  SdSpeedTestSnapshot _snapshot;
  uint32_t _lastProgressPublishMs;
  SdThroughputBenchmarkPhase _lastBenchmarkPhase;

  static void taskMain(void *context);
  void run(uint32_t generation);
  bool current(uint32_t generation) const;
  void publish(SdSpeedTestPhase phase, uint8_t progress, const char *status,
               float writeMiBps = -1.0f, float readMiBps = -1.0f,
               bool verified = false);

  bool sdBenchmarkCancelled() const override;
  void sdBenchmarkProgress(SdThroughputBenchmarkPhase phase,
                           uint32_t completedBytes,
                           uint32_t totalBytes) override;
};
