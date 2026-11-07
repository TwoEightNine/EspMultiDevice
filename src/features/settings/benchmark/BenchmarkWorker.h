#pragma once

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "BenchmarkScenes.h"

// Screen-owned renderer for Settings -> Benchmark. A lead task on the core
// opposite the Arduino loop runs the timeline and per-frame setup; a helper
// task on the loop's core joins band rendering when a stage uses two cores.
// Finished frames are published in 64-byte-aligned PSRAM slots that the UI
// presents with DisplayController DMA. Neither task draws to the TFT or
// navigates.
class BenchmarkWorker {
public:
  static constexpr uint8_t StageCount = 5;
  static constexpr uint8_t SlotCount = 3;
  static constexpr int16_t MaxHeight = 384;

  enum class Mode : uint8_t { Idle, Preview, Run, Score };

  struct Frame {
    uint8_t slot = 0;
    const uint16_t *pixels = nullptr;
    int16_t height = 0;
  };

  struct Snapshot {
    Mode mode = Mode::Idle;
    bool active = false;
    bool finished = false;
    bool failed = false;
    uint8_t stage = 0;
    uint8_t progress = 0;
    float liveFps = 0.0f;
    float stageFps[StageCount] = {};
    uint32_t revision = 0;
    uint32_t generation = 0;
  };

  ~BenchmarkWorker();

  bool begin();
  // Returns false until both tasks have stopped; call again from tick().
  // The caller must not hold a frame borrowed through takeFrame().
  bool end();
  bool started() const { return _lead != nullptr; }

  bool startPreview(int16_t height);
  bool startRun(int16_t height);
  bool startScore(int16_t height, uint32_t score);
  void cancel();

  // Takes the oldest finished frame of the current job, if any.
  bool takeFrame(Frame &frame);
  void releaseFrame(uint8_t slot);
  Snapshot snapshot() const;

  static const char *stageName(uint8_t stage);
  static uint8_t stageCores(uint8_t stage);
  static uint32_t totalRunMs();
  // 1000 is the reference device at 240 MHz with its stock wiring.
  static uint32_t score(const float stageFps[StageCount]);

private:
  enum SlotState : uint8_t { SlotFree, SlotRendering, SlotReady, SlotPresenting };

  struct Slot {
    uint16_t *pixels = nullptr;
    std::atomic<uint8_t> state{SlotFree};
    uint32_t sequence = 0;
    uint32_t generation = 0;
    int16_t height = 0;
  };

  BenchmarkScenes _scenes;
  Slot _slots[SlotCount];
  uint16_t *_depth[2] = {nullptr, nullptr};
  TaskHandle_t _lead = nullptr;
  TaskHandle_t _helper = nullptr;
  std::atomic<bool> _leadAlive{false};
  std::atomic<bool> _helperAlive{false};
  std::atomic<bool> _stopTasks{false};
  std::atomic<uint32_t> _generation{0};

  // Job request (UI -> lead), guarded by _lock.
  Mode _requestMode = Mode::Idle;
  int16_t _requestHeight = 0;
  uint32_t _requestScore = 0;

  // Band dispatch: high 16 bits frame token, low 16 bits next band index.
  std::atomic<uint32_t> _work{0};
  std::atomic<uint16_t> _bandsDone{0};
  uint16_t _frameToken = 0;
  BenchScene _frameScene = BenchScene::None;
  uint16_t *_framePixels = nullptr;
  int16_t _frameHeight = 0;
  uint16_t _bandCount = 0;
  uint32_t _frameSequence = 0;

  mutable portMUX_TYPE _lock = portMUX_INITIALIZER_UNLOCKED;
  Snapshot _snapshot;

  static void leadMain(void *context);
  static void helperMain(void *context);
  void leadLoop();
  void helperLoop();
  void runJob(Mode mode, int16_t height, uint32_t score, uint32_t generation);
  bool runStages(int16_t height, uint32_t generation, Snapshot &state);
  bool renderFrame(BenchScene scene, const BenchFrameParams &params, uint8_t cores, uint32_t generation);
  void renderBands(uint16_t token, uint16_t *depth);
  int acquireSlot(uint32_t generation);
  bool current(uint32_t generation) const;
  void publish(const Snapshot &state);
  bool request(Mode mode, int16_t height, uint32_t score);
  void freeBuffers();
};
