#include "BenchmarkWorker.h"

#include <esp_heap_caps.h>
#include <math.h>

namespace {

constexpr uint32_t FrameAlignment = 64;
constexpr uint32_t LeadStackBytes = 8192;
constexpr uint32_t HelperStackBytes = 6144;
constexpr UBaseType_t TaskPriority = 1;
constexpr uint32_t YieldIntervalUs = 100000;
constexpr uint32_t PublishIntervalUs = 200000;
constexpr uint32_t FpsWindowUs = 400000;
constexpr float ScoreAnimationSeconds = 2.4f;
constexpr float ScoreCountSeconds = 1.3f;
constexpr float PreviewTime = 2.2f;

struct StageInfo {
  BenchScene scene;
  uint16_t durationMs;
  uint8_t cores;
  float timeOffset;
  const char *name;
  // Average FPS of the reference device (ESP32-S3 at 240 MHz, 20 MHz LCD
  // pixel clock). Recalibrate these together whenever a scene changes.
  float referenceFps;
};

const StageInfo Stages[BenchmarkWorker::StageCount] = {
  {BenchScene::Warp, 8000, 2, 0.0f, "Star warp", 40.0f},
  {BenchScene::Crystal, 10000, 2, 0.0f, "Crystals", 24.0f},
  {BenchScene::Knot, 7000, 1, 0.0f, "Knot \xC2\xB7 1 core", 12.0f},
  {BenchScene::Knot, 7000, 2, 7.0f, "Knot \xC2\xB7 2 cores", 20.0f},
  {BenchScene::Tunnel, 10000, 2, 0.0f, "Plasma tunnel", 30.0f},
};

float easeOut(float t) {
  t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
  const float inverse = 1.0f - t;
  return 1.0f - inverse * inverse * inverse;
}

}  // namespace

BenchmarkWorker::~BenchmarkWorker() {
  for (uint16_t attempt = 0; attempt < 1000 && !end(); ++attempt) vTaskDelay(1);
}

const char *BenchmarkWorker::stageName(uint8_t stage) {
  return stage < StageCount ? Stages[stage].name : "";
}

uint8_t BenchmarkWorker::stageCores(uint8_t stage) {
  return stage < StageCount ? Stages[stage].cores : 0;
}

uint32_t BenchmarkWorker::totalRunMs() {
  uint32_t total = 0;
  for (uint8_t i = 0; i < StageCount; ++i) total += Stages[i].durationMs;
  return total;
}

uint32_t BenchmarkWorker::score(const float stageFps[StageCount]) {
  float logSum = 0.0f;
  for (uint8_t i = 0; i < StageCount; ++i) {
    if (stageFps[i] <= 0.0f) return 0;
    logSum += logf(stageFps[i] / Stages[i].referenceFps);
  }
  const float value = 1000.0f * expf(logSum / StageCount) + 0.5f;
  return value > 99999.0f ? 99999u : (uint32_t)value;
}

bool BenchmarkWorker::begin() {
  if (_lead) return true;
  const size_t frameBytes = (size_t)BenchRaster::Width * MaxHeight * sizeof(uint16_t);
  for (uint8_t i = 0; i < SlotCount; ++i) {
    _slots[i].pixels = static_cast<uint16_t *>(
      heap_caps_aligned_alloc(FrameAlignment, frameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    _slots[i].state.store(SlotFree);
  }
  const size_t depthBytes = (size_t)BenchRaster::Width * BenchRaster::BandHeight * sizeof(uint16_t);
  for (uint8_t i = 0; i < 2; ++i) {
    _depth[i] = static_cast<uint16_t *>(heap_caps_malloc(depthBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!_depth[i]) _depth[i] = static_cast<uint16_t *>(heap_caps_malloc(depthBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  bool ready = _depth[0] && _depth[1] && _scenes.begin();
  for (uint8_t i = 0; i < SlotCount; ++i) ready = ready && _slots[i].pixels;
  if (!ready) {
    Serial.println(F("[Benchmark] Not enough memory"));
    freeBuffers();
    return false;
  }

  _stopTasks.store(false);
  _snapshot = Snapshot();
  const BaseType_t uiCore = xPortGetCoreID();
  const BaseType_t otherCore = uiCore == 0 ? 1 : 0;
  _leadAlive.store(true);
  if (xTaskCreatePinnedToCore(leadMain, "bench-lead", LeadStackBytes, this, TaskPriority, &_lead, otherCore) != pdPASS) {
    _leadAlive.store(false);
    _lead = nullptr;
    freeBuffers();
    return false;
  }
  _helperAlive.store(true);
  if (xTaskCreatePinnedToCore(helperMain, "bench-help", HelperStackBytes, this, TaskPriority, &_helper, uiCore) != pdPASS) {
    // The lead renders every band alone if the helper is unavailable.
    _helperAlive.store(false);
    _helper = nullptr;
  }
  return true;
}

bool BenchmarkWorker::end() {
  if (!_lead && !_helper) {
    freeBuffers();
    return true;
  }
  if (!_stopTasks.load()) {
    _stopTasks.store(true);
    _generation.fetch_add(1);
    if (_lead) xTaskNotifyGive(_lead);
    if (_helper) xTaskNotifyGive(_helper);
  }
  if (_leadAlive.load() || _helperAlive.load()) return false;
  _lead = nullptr;
  _helper = nullptr;
  freeBuffers();
  _stopTasks.store(false);
  return true;
}

void BenchmarkWorker::freeBuffers() {
  _scenes.end();
  for (uint8_t i = 0; i < SlotCount; ++i) {
    if (_slots[i].pixels) heap_caps_free(_slots[i].pixels);
    _slots[i].pixels = nullptr;
    _slots[i].state.store(SlotFree);
  }
  for (uint8_t i = 0; i < 2; ++i) {
    if (_depth[i]) heap_caps_free(_depth[i]);
    _depth[i] = nullptr;
  }
}

bool BenchmarkWorker::request(Mode mode, int16_t height, uint32_t score) {
  if (!_lead || height <= 0 || height > MaxHeight) return false;
  portENTER_CRITICAL(&_lock);
  _requestMode = mode;
  _requestHeight = height;
  _requestScore = score;
  const uint32_t generation = _generation.fetch_add(1) + 1;
  _snapshot = Snapshot();
  _snapshot.mode = mode;
  _snapshot.active = true;
  _snapshot.generation = generation;
  portEXIT_CRITICAL(&_lock);
  xTaskNotifyGive(_lead);
  return true;
}

bool BenchmarkWorker::startPreview(int16_t height) {
  return request(Mode::Preview, height, 0);
}

bool BenchmarkWorker::startRun(int16_t height) {
  return request(Mode::Run, height, 0);
}

bool BenchmarkWorker::startScore(int16_t height, uint32_t score) {
  return request(Mode::Score, height, score);
}

void BenchmarkWorker::cancel() {
  portENTER_CRITICAL(&_lock);
  _requestMode = Mode::Idle;
  const uint32_t generation = _generation.fetch_add(1) + 1;
  _snapshot = Snapshot();
  _snapshot.generation = generation;
  portEXIT_CRITICAL(&_lock);
}

bool BenchmarkWorker::takeFrame(Frame &frame) {
  const uint32_t generation = _generation.load();
  int8_t best = -1;
  for (uint8_t i = 0; i < SlotCount; ++i) {
    Slot &slot = _slots[i];
    if (slot.state.load(std::memory_order_acquire) != SlotReady) continue;
    if (slot.generation != generation) {
      uint8_t expected = SlotReady;
      slot.state.compare_exchange_strong(expected, SlotFree);
      continue;
    }
    if (best < 0 || slot.sequence < _slots[best].sequence) best = i;
  }
  if (best < 0) return false;
  uint8_t expected = SlotReady;
  if (!_slots[best].state.compare_exchange_strong(expected, SlotPresenting)) return false;
  frame.slot = best;
  frame.pixels = _slots[best].pixels;
  frame.height = _slots[best].height;
  return true;
}

void BenchmarkWorker::releaseFrame(uint8_t slot) {
  if (slot < SlotCount) _slots[slot].state.store(SlotFree, std::memory_order_release);
}

BenchmarkWorker::Snapshot BenchmarkWorker::snapshot() const {
  portENTER_CRITICAL(&_lock);
  const Snapshot copy = _snapshot;
  portEXIT_CRITICAL(&_lock);
  return copy;
}

bool BenchmarkWorker::current(uint32_t generation) const {
  return !_stopTasks.load() && _generation.load() == generation;
}

void BenchmarkWorker::publish(const Snapshot &state) {
  portENTER_CRITICAL(&_lock);
  if (_snapshot.generation == state.generation) {
    const uint32_t revision = _snapshot.revision + 1;
    _snapshot = state;
    _snapshot.revision = revision;
  }
  portEXIT_CRITICAL(&_lock);
}

void BenchmarkWorker::leadMain(void *context) {
  static_cast<BenchmarkWorker *>(context)->leadLoop();
}

void BenchmarkWorker::helperMain(void *context) {
  static_cast<BenchmarkWorker *>(context)->helperLoop();
}

void BenchmarkWorker::leadLoop() {
  while (!_stopTasks.load()) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
    if (_stopTasks.load()) break;
    portENTER_CRITICAL(&_lock);
    const Mode mode = _requestMode;
    const int16_t height = _requestHeight;
    const uint32_t score = _requestScore;
    const uint32_t generation = _generation.load();
    _requestMode = Mode::Idle;
    portEXIT_CRITICAL(&_lock);
    if (mode != Mode::Idle) runJob(mode, height, score, generation);
  }
  _leadAlive.store(false);
  vTaskDelete(nullptr);
}

void BenchmarkWorker::helperLoop() {
  while (!_stopTasks.load()) {
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50)) == 0) continue;
    if (_stopTasks.load()) break;
    const uint16_t token = (uint16_t)(_work.load(std::memory_order_acquire) >> 16);
    renderBands(token, _depth[1]);
  }
  _helperAlive.store(false);
  vTaskDelete(nullptr);
}

void BenchmarkWorker::runJob(Mode mode, int16_t height, uint32_t score, uint32_t generation) {
  Snapshot state;
  state.mode = mode;
  state.active = true;
  state.generation = generation;
  publish(state);

  bool ok = true;
  if (mode == Mode::Preview) {
    BenchFrameParams params;
    params.t = PreviewTime;
    params.height = height;
    ok = _scenes.prepare(BenchScene::Knot, height) && renderFrame(BenchScene::Knot, params, 2, generation);
  } else if (mode == Mode::Score) {
    ok = _scenes.prepare(BenchScene::Score, height);
    BenchFrameParams params;
    params.height = height;
    params.digits = 1;
    for (uint32_t value = score; value >= 10; value /= 10) ++params.digits;
    const uint32_t startUs = micros();
    while (ok) {
      float t = (micros() - startUs) / 1000000.0f;
      const bool last = t >= ScoreAnimationSeconds;
      if (last) t = ScoreAnimationSeconds;
      params.t = t;
      params.value = last ? score : (uint32_t)(score * easeOut(t / ScoreCountSeconds) + 0.5f);
      if (params.value > score) params.value = score;
      ok = renderFrame(BenchScene::Score, params, 2, generation);
      if (last) break;
      vTaskDelay(1);
    }
  } else if (mode == Mode::Run) {
    ok = runStages(height, generation, state);
  }

  if (!current(generation)) return;
  state.active = false;
  state.finished = ok;
  state.failed = !ok;
  if (mode == Mode::Run && ok) state.progress = 100;
  publish(state);
}

bool BenchmarkWorker::runStages(int16_t height, uint32_t generation, Snapshot &state) {
  const uint32_t totalMs = totalRunMs();
  uint32_t completedMs = 0;
  BenchScene prepared = BenchScene::None;
  for (uint8_t index = 0; index < StageCount; ++index) {
    const StageInfo &stage = Stages[index];
    state.stage = index;
    state.progress = (uint8_t)(completedMs * 100 / totalMs);
    publish(state);
    if (stage.scene != prepared) {
      if (!_scenes.prepare(stage.scene, height)) return false;
      prepared = stage.scene;
    }
    BenchFrameParams params;
    params.height = height;
    const uint32_t durationUs = (uint32_t)stage.durationMs * 1000u;
    const uint32_t startUs = micros();
    uint32_t lastDoneUs = startUs;
    uint32_t windowStartUs = startUs;
    uint32_t lastPublishUs = startUs;
    uint32_t lastYieldUs = startUs;
    uint32_t frames = 0;
    uint16_t windowFrames = 0;
    while (true) {
      if (!current(generation)) return false;
      const uint32_t elapsedUs = micros() - startUs;
      if (elapsedUs >= durationUs) break;
      const float elapsed = elapsedUs / 1000000.0f;
      params.t = stage.timeOffset + elapsed;
      params.flash = stage.timeOffset > 0.0f ? fmaxf(0.0f, 1.0f - elapsed / 0.8f) : 0.0f;
      if (!renderFrame(stage.scene, params, stage.cores, generation)) return false;
      const uint32_t nowUs = micros();
      ++frames;
      ++windowFrames;
      lastDoneUs = nowUs;
      if (nowUs - windowStartUs >= FpsWindowUs) {
        state.liveFps = windowFrames * 1000000.0f / (nowUs - windowStartUs);
        windowStartUs = nowUs;
        windowFrames = 0;
      }
      if (nowUs - lastPublishUs >= PublishIntervalUs) {
        state.progress = (uint8_t)((completedMs + (nowUs - startUs) / 1000u) * 100u / totalMs);
        publish(state);
        lastPublishUs = nowUs;
      }
      // The core-0 idle task feeds the task watchdog.
      if (nowUs - lastYieldUs >= YieldIntervalUs) {
        vTaskDelay(1);
        lastYieldUs = micros();
      }
    }
    state.stageFps[index] = frames && lastDoneUs != startUs ? frames * 1000000.0f / (lastDoneUs - startUs) : 0.0f;
    completedMs += stage.durationMs;
    Serial.printf("[Benchmark] %s: %.1f fps\n", stage.name, state.stageFps[index]);
  }
  Serial.printf("[Benchmark] score %lu\n", static_cast<unsigned long>(score(state.stageFps)));
  return true;
}

int BenchmarkWorker::acquireSlot(uint32_t generation) {
  while (current(generation)) {
    for (uint8_t i = 0; i < SlotCount; ++i) {
      Slot &slot = _slots[i];
      uint8_t expected = SlotFree;
      if (slot.state.compare_exchange_strong(expected, SlotRendering)) return i;
      // Frames of an older job are never presented; reclaim them.
      expected = SlotReady;
      if (slot.generation != generation && slot.state.compare_exchange_strong(expected, SlotRendering)) return i;
    }
    vTaskDelay(1);
  }
  return -1;
}

bool BenchmarkWorker::renderFrame(BenchScene scene, const BenchFrameParams &params, uint8_t cores,
                                  uint32_t generation) {
  const int slot = acquireSlot(generation);
  if (slot < 0) return false;
  _scenes.setupFrame(scene, params);

  _frameScene = scene;
  _framePixels = _slots[slot].pixels;
  _frameHeight = params.height;
  _bandCount = (params.height + BenchRaster::BandHeight - 1) / BenchRaster::BandHeight;
  _bandsDone.store(0, std::memory_order_relaxed);
  if (++_frameToken == 0) ++_frameToken;
  const uint16_t token = _frameToken;
  _work.store((uint32_t)token << 16, std::memory_order_release);
  if (cores > 1 && _helper) xTaskNotifyGive(_helper);
  renderBands(token, _depth[0]);

  const uint32_t waitStartUs = micros();
  while (_bandsDone.load(std::memory_order_acquire) < _bandCount) {
    if (_stopTasks.load()) {
      // The helper exits only between bands, so wait for it to stop writing.
      while (_helperAlive.load() && _bandsDone.load() < _bandCount) vTaskDelay(1);
      _slots[slot].state.store(SlotFree);
      return false;
    }
    if (micros() - waitStartUs > 20000) vTaskDelay(1);
  }

  Slot &target = _slots[slot];
  target.height = params.height;
  target.sequence = ++_frameSequence;
  target.generation = generation;
  target.state.store(SlotReady, std::memory_order_release);
  return true;
}

void BenchmarkWorker::renderBands(uint16_t token, uint16_t *depth) {
  while (true) {
    uint32_t work = _work.load(std::memory_order_acquire);
    uint16_t band;
    while (true) {
      if ((uint16_t)(work >> 16) != token) return;
      band = (uint16_t)(work & 0xFFFF);
      if (band >= _bandCount) return;
      if (_work.compare_exchange_weak(work, work + 1, std::memory_order_acq_rel)) break;
    }
    BenchRaster::Band target;
    target.y0 = band * BenchRaster::BandHeight;
    target.rows = min<int16_t>(BenchRaster::BandHeight, _frameHeight - target.y0);
    target.pixels = _framePixels + (int32_t)target.y0 * BenchRaster::Width;
    target.depth = depth;
    _scenes.renderBand(_frameScene, target);
    _bandsDone.fetch_add(1, std::memory_order_release);
  }
}
