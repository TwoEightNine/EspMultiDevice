#include "AnimationStreamWorker.h"

#include "../../../third_party/AnimatedGIF/AnimatedGIF.h"
#include "../../core/SpiBusLock.h"

#include <esp32-hal-psram.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <stdio.h>
#include <string.h>

namespace {
constexpr uint32_t WorkerStackBytes = 8192;
constexpr size_t SlotAlignment = 64;
// A frame this late is decoded for the canvas but neither scaled nor
// published, unless nothing has been published for as long.
constexpr uint32_t LateMs = 250;
constexpr uint32_t LockRetryMs = 20;
constexpr uint32_t SourceCloseLockMs = 250;
// GIF delays of 10 ms or less play at 100 ms, as browsers do. AnimatedGIF
// already applies this to explicit delays; frames without a graphic control
// extension report zero.
constexpr uint32_t ShortDelayMs = 10;
constexpr uint32_t ShortDelayReplacementMs = 100;

class LockGuard {
public:
  explicit LockGuard(SemaphoreHandle_t lock) : _lock(lock) { xSemaphoreTake(_lock, portMAX_DELAY); }
  ~LockGuard() { xSemaphoreGive(_lock); }

private:
  SemaphoreHandle_t _lock;
};
}  // namespace

AnimationStreamWorker *volatile AnimationStreamWorker::_current = nullptr;

AnimationStreamWorker::AnimationStreamWorker(SecretStorage &storage, AnimatedGIF &decoder)
    : _storage(storage), _decoder(decoder), _secretAccess(false), _task(nullptr), _lock(nullptr),
      _stopped(nullptr), _stopRequested(false), _decoderOpen(false), _sourceWidth(0),
      _sourceHeight(0), _canvasPixels(0), _drawWidth(0), _drawHeight(0), _columnMap(nullptr),
      _rowMap(nullptr), _loopCount(-1), _slots{}, _timelineStarted(false), _timelineStartMs(0),
      _lastPublishedMs(0), _phase(Phase::Idle), _error{}, _decoded(0), _dropped(0),
      _decodeTotalUs(0), _decodeMaxUs(0), _scaleTotalUs(0), _scaled(0), _stackHighWater(0) {}

AnimationStreamWorker::~AnimationStreamWorker() {
  stop();
  if (_lock) vSemaphoreDelete(_lock);
}

bool AnimationStreamWorker::start(const String &path, bool secretAccess, uint32_t sourceWidth,
                                  uint32_t sourceHeight, uint16_t drawWidth, uint16_t drawHeight,
                                  String &error) {
  stop();
  if (!_lock) _lock = xSemaphoreCreateMutex();
  _stopped = xSemaphoreCreateBinary();
  if (!_lock || !_stopped) {
    error = F("Animation worker setup failed");
    stop();
    return false;
  }
  if (!sourceWidth || !sourceHeight || !drawWidth || !drawHeight) {
    error = F("Invalid animation size");
    stop();
    return false;
  }
  // AnimatedGIF's cooked canvas keeps indexed, RGB565, and previous-frame
  // copies plus a spare row: five bytes per canvas pixel and three per column.
  const uint64_t canvasBytes = (uint64_t)sourceWidth * sourceHeight * 5ULL + (uint64_t)sourceWidth * 3ULL;
  const uint32_t slotBytes = (uint32_t)drawWidth * drawHeight * sizeof(uint16_t);
  const uint32_t freePsram = ESP.getFreePsram();
  const uint32_t largestPsram = ESP.getMaxAllocPsram();
  if (canvasBytes + (uint64_t)slotBytes * SlotCount > freePsram || canvasBytes > largestPsram) {
    error = String(F("Animation needs too much PSRAM (")) +
            String((uint32_t)((canvasBytes + (uint64_t)slotBytes * SlotCount) / 1024ULL)) +
            F(" KB; free ") + String(freePsram / 1024) + F(" KB, largest ") +
            String(largestPsram / 1024) + F(" KB)");
    stop();
    return false;
  }
  for (uint8_t index = 0; index < SlotCount; ++index) {
    _slots[index].pixels = static_cast<uint16_t *>(
        heap_caps_aligned_alloc(SlotAlignment, slotBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!_slots[index].pixels) {
      error = String(F("Animation frames need ")) + String((slotBytes * SlotCount) / 1024) +
              F(" KB of PSRAM; largest ") + String(ESP.getMaxAllocPsram() / 1024) + F(" KB");
      stop();
      return false;
    }
    _slots[index].state = SlotState::Free;
  }
  _path = path;
  _secretAccess = secretAccess;
  _sourceWidth = sourceWidth;
  _sourceHeight = sourceHeight;
  _drawWidth = drawWidth;
  _drawHeight = drawHeight;
  _stopRequested = false;
  _phase = Phase::Running;
#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
  const BaseType_t workerCore = 0;
#else
  const BaseType_t workerCore = ARDUINO_RUNNING_CORE == 0 ? 1 : 0;
#endif
  if (xTaskCreatePinnedToCore(taskMain, "anim-stream", WorkerStackBytes, this, tskIDLE_PRIORITY + 1,
                              &_task, workerCore) != pdPASS) {
    _task = nullptr;
    error = F("Animation worker could not start");
    stop();
    return false;
  }
  return true;
}

void AnimationStreamWorker::stop() {
  if (_task) {
    _stopRequested = true;
    xTaskNotifyGive(_task);
    // The worker checks the flag between frames and inside every source
    // read, and releases the file and decoder before acknowledging. Never
    // free a slot before that point.
    while (xSemaphoreTake(_stopped, pdMS_TO_TICKS(25)) != pdTRUE) {
    }
    _task = nullptr;
    Serial.printf("[Anim] worker stack free %u bytes\n", (unsigned)_stackHighWater);
  }
  if (_stopped) vSemaphoreDelete(_stopped);
  _stopped = nullptr;
  freeSlots();
  _path = "";
  _stopRequested = false;
  _timelineStarted = false;
  _timelineStartMs = 0;
  _lastPublishedMs = 0;
  _phase = Phase::Idle;
  _error[0] = '\0';
  _decoded = 0;
  _dropped = 0;
  _decodeTotalUs = 0;
  _decodeMaxUs = 0;
  _scaleTotalUs = 0;
  _scaled = 0;
  _stackHighWater = 0;
}

void AnimationStreamWorker::freeSlots() {
  for (uint8_t index = 0; index < SlotCount; ++index) {
    if (_slots[index].pixels) heap_caps_free(_slots[index].pixels);
    _slots[index] = {};
  }
}

bool AnimationStreamWorker::takeDueFrame(uint32_t nowMs, Frame &frame) {
  if (!_task || !_lock) return false;
  uint8_t chosen = NoSlot;
  bool released = false;
  {
    LockGuard guard(_lock);
    for (uint8_t index = 0; index < SlotCount; ++index) {
      const Slot &slot = _slots[index];
      if (slot.state != SlotState::Ready) continue;
      if (!_timelineStarted) {
        if (chosen == NoSlot || slot.sequence < _slots[chosen].sequence) chosen = index;
      } else if ((int32_t)(nowMs - (_timelineStartMs + slot.dueMs)) >= 0 &&
                 (chosen == NoSlot || slot.sequence > _slots[chosen].sequence)) {
        chosen = index;
      }
    }
    if (chosen == NoSlot) return false;
    for (uint8_t index = 0; index < SlotCount; ++index) {
      Slot &slot = _slots[index];
      if (index == chosen || slot.state != SlotState::Ready ||
          slot.sequence > _slots[chosen].sequence) continue;
      slot.state = SlotState::Free;
      ++_dropped;
      released = true;
    }
    Slot &slot = _slots[chosen];
    slot.state = SlotState::Displayed;
    frame.slot = chosen;
    frame.pixels = slot.pixels;
    frame.sequence = slot.sequence;
    frame.dueMs = slot.dueMs;
  }
  if (released) xTaskNotifyGive(_task);
  return true;
}

void AnimationStreamWorker::releaseFrame(uint8_t slot) {
  if (!_lock || slot >= SlotCount) return;
  {
    LockGuard guard(_lock);
    if (_slots[slot].state == SlotState::Displayed) _slots[slot].state = SlotState::Free;
  }
  if (_task) xTaskNotifyGive(_task);
}

void AnimationStreamWorker::startTimeline(uint32_t startMs) {
  if (!_lock) return;
  LockGuard guard(_lock);
  _timelineStartMs = startMs;
  _timelineStarted = true;
}

AnimationStreamWorker::Stats AnimationStreamWorker::stats(String *error) {
  Stats copy;
  if (!_lock) return copy;
  LockGuard guard(_lock);
  copy.phase = _phase;
  copy.decoded = _decoded;
  copy.dropped = _dropped;
  copy.decodeAverageUs = _decoded ? (uint32_t)(_decodeTotalUs / _decoded) : 0;
  copy.decodeMaxUs = _decodeMaxUs;
  copy.scaleAverageUs = _scaled ? (uint32_t)(_scaleTotalUs / _scaled) : 0;
  if (error) *error = _error;
  return copy;
}

void AnimationStreamWorker::taskMain(void *context) {
  static_cast<AnimationStreamWorker *>(context)->run();
}

void AnimationStreamWorker::run() {
  // AnimatedGIF's callbacks carry no context. Only one worker runs at a
  // time, and only its task invokes them.
  _current = this;
  if (openDecoder()) decodeLoop();
  // Release the source and decoder as soon as playback ends or fails; the
  // UI keeps showing its slot until it stops the worker.
  closeDecoder();
  while (!_stopRequested) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
  _stackHighWater = uxTaskGetStackHighWaterMark(nullptr);
  _current = nullptr;
  xSemaphoreGive(_stopped);
  vTaskDelete(nullptr);
}

bool AnimationStreamWorker::openDecoder() {
  _decoder.begin(GIF_PALETTE_RGB565_LE);
  const int opened = _decoder.open(_path.c_str(), gifOpen, gifClose, gifRead, gifSeek, nullptr);
  if (!opened) {
    if (!_stopRequested) {
      char message[48];
      snprintf(message, sizeof(message), "GIF header error %d", _decoder.getLastError());
      fail(message);
    }
    if (_file) gifClose(&_file);
    return false;
  }
  _decoderOpen = true;
  if ((uint32_t)_decoder.getCanvasWidth() != _sourceWidth ||
      (uint32_t)_decoder.getCanvasHeight() != _sourceHeight) {
    fail("GIF header changed while opening");
    return false;
  }
  _canvasPixels = _sourceWidth * _sourceHeight;
  // A null draw callback makes AnimatedGIF keep the complete cooked RGB565
  // canvas, including disposal and transparency, in its frame buffer.
  _decoder.setDrawType(GIF_DRAW_COOKED);
  if (_decoder.allocFrameBuf(psramAlloc) != GIF_SUCCESS) {
    char message[96];
    snprintf(message, sizeof(message), "GIF canvas needs too much PSRAM (largest %u KB)",
             (unsigned)(ESP.getMaxAllocPsram() / 1024));
    fail(message);
    return false;
  }
  clearCanvas();
  _loopCount = _decoder.getLoopCount();
  _columnMap = static_cast<uint16_t *>(
      heap_caps_malloc((size_t)_drawWidth * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  _rowMap = static_cast<uint16_t *>(
      heap_caps_malloc((size_t)_drawHeight * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!_columnMap || !_rowMap) {
    fail("Animation scaler needs more memory");
    return false;
  }
  for (uint32_t x = 0; x < _drawWidth; ++x) {
    _columnMap[x] = (uint16_t)(_drawWidth > 1 ? (x * (_sourceWidth - 1)) / (_drawWidth - 1) : 0);
  }
  for (uint32_t y = 0; y < _drawHeight; ++y) {
    _rowMap[y] = (uint16_t)(_drawHeight > 1 ? (y * (_sourceHeight - 1)) / (_drawHeight - 1) : 0);
  }
  return true;
}

void AnimationStreamWorker::closeDecoder() {
  if (_decoderOpen) {
    _decoder.close();
    _decoderOpen = false;
  }
  if (_decoder.getFrameBuf()) _decoder.freeFrameBuf(psramFree);
  if (_file) gifClose(&_file);
  if (_columnMap) heap_caps_free(_columnMap);
  if (_rowMap) heap_caps_free(_rowMap);
  _columnMap = nullptr;
  _rowMap = nullptr;
}

void AnimationStreamWorker::clearCanvas() {
  uint8_t *canvas = _decoder.getFrameBuf();
  if (canvas) memset(canvas, 0, (size_t)_canvasPixels * 5 + (size_t)_sourceWidth * 3);
}

void AnimationStreamWorker::decodeLoop() {
  // NETSCAPE loop count: none plays once, zero repeats forever, and N
  // repeats N more times.
  const uint32_t totalPasses = _loopCount < 0 ? 1 : (uint32_t)_loopCount + 1;
  uint32_t passes = 0;
  uint32_t passFrames = 0;
  uint32_t sequence = 0;
  uint32_t dueMs = 0;
  bool reportedTruncation = false;
  while (!_stopRequested) {
    uint8_t slot = acquireSlot();
    while (slot == NoSlot && !_stopRequested) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
      slot = acquireSlot();
    }
    if (_stopRequested) {
      if (slot != NoSlot) returnSlot(slot);
      return;
    }

    int delay = 0;
    const int64_t decodeStarted = esp_timer_get_time();
    const int result = _decoder.playFrame(false, &delay, nullptr);
    const uint32_t decodeUs = (uint32_t)(esp_timer_get_time() - decodeStarted);
    if (_stopRequested) {
      returnSlot(slot);
      return;
    }
    const int status = _decoder.getLastError();
    bool frameDecoded = false;
    bool endOfPass = false;
    if (result < 0 || (status != GIF_SUCCESS && status != GIF_EMPTY_FRAME)) {
      if (passFrames == 0) {
        returnSlot(slot);
        char message[48];
        snprintf(message, sizeof(message), "GIF decode error %d", status);
        fail(message);
        return;
      }
      // A truncated or damaged file loops the frames that did decode, as
      // browsers do.
      if (!reportedTruncation) {
        Serial.printf("[Anim] GIF stream ended early after %lu frame(s), error %d\n",
                      (unsigned long)passFrames, status);
        reportedTruncation = true;
      }
      endOfPass = true;
    } else if (status == GIF_EMPTY_FRAME) {
      endOfPass = true;
    } else {
      frameDecoded = true;
      endOfPass = result == 0;
    }

    const bool finalPass = _loopCount != 0 && passes + 1 >= totalPasses;
    // A single-frame source never needs a second decode.
    const bool finishing = endOfPass && (finalPass || (passFrames + (frameDecoded ? 1 : 0)) <= 1);
    if (frameDecoded) {
      {
        LockGuard guard(_lock);
        ++_decoded;
        _decodeTotalUs += decodeUs;
        if (decodeUs > _decodeMaxUs) _decodeMaxUs = decodeUs;
      }
      const uint32_t nowMs = millis();
      // The final frame always appears; otherwise a stale frame is skipped
      // unless the display has gone a full LateMs without a new one.
      if (!finishing && timelineLate(dueMs, nowMs)) {
        returnSlot(slot);
        LockGuard guard(_lock);
        ++_dropped;
      } else {
        const int64_t scaleStarted = esp_timer_get_time();
        scaleInto(_slots[slot].pixels);
        const uint32_t scaleUs = (uint32_t)(esp_timer_get_time() - scaleStarted);
        {
          LockGuard guard(_lock);
          _scaleTotalUs += scaleUs;
          ++_scaled;
        }
        publish(slot, sequence, dueMs);
        _lastPublishedMs = nowMs;
      }
      ++sequence;
      ++passFrames;
      const uint32_t frameDelay = (uint32_t)(delay > 0 ? delay : 0);
      dueMs += frameDelay <= ShortDelayMs ? ShortDelayReplacementMs : frameDelay;
    } else {
      returnSlot(slot);
    }

    if (endOfPass) {
      if (finishing) {
        LockGuard guard(_lock);
        if (_phase == Phase::Running) _phase = Phase::Finished;
        return;
      }
      ++passes;
      passFrames = 0;
      _decoder.reset();
      // Each pass starts from an empty canvas, like the first.
      clearCanvas();
    }
    // Let the idle task run between frames on this core.
    vTaskDelay(1);
  }
}

void AnimationStreamWorker::scaleInto(uint16_t *destination) {
  const uint8_t *cooked = _decoder.getFrameBuf() + _canvasPixels;
  const uint32_t pitch = _sourceWidth * 2;
  // The cooked canvas follows the one-byte-per-pixel indexed canvas, so an
  // odd pixel count leaves it unaligned for 16-bit loads.
  const bool aligned = (reinterpret_cast<uintptr_t>(cooked) & 1U) == 0;
  for (uint32_t y = 0; y < _drawHeight; ++y) {
    const uint8_t *row = cooked + (size_t)_rowMap[y] * pitch;
    uint16_t *out = destination + (size_t)y * _drawWidth;
    if (aligned) {
      const uint16_t *row16 = reinterpret_cast<const uint16_t *>(row);
      for (uint32_t x = 0; x < _drawWidth; ++x) out[x] = row16[_columnMap[x]];
    } else {
      for (uint32_t x = 0; x < _drawWidth; ++x) {
        const uint8_t *pixel = row + (size_t)_columnMap[x] * 2;
        out[x] = (uint16_t)(pixel[0] | (pixel[1] << 8));
      }
    }
  }
}

uint8_t AnimationStreamWorker::acquireSlot() {
  LockGuard guard(_lock);
  for (uint8_t index = 0; index < SlotCount; ++index) {
    if (_slots[index].state == SlotState::Free) {
      _slots[index].state = SlotState::Writing;
      return index;
    }
  }
  return NoSlot;
}

void AnimationStreamWorker::publish(uint8_t slot, uint32_t sequence, uint32_t dueMs) {
  LockGuard guard(_lock);
  _slots[slot].sequence = sequence;
  _slots[slot].dueMs = dueMs;
  _slots[slot].state = SlotState::Ready;
}

void AnimationStreamWorker::returnSlot(uint8_t slot) {
  LockGuard guard(_lock);
  if (_slots[slot].state == SlotState::Writing) _slots[slot].state = SlotState::Free;
}

bool AnimationStreamWorker::timelineLate(uint32_t dueMs, uint32_t nowMs) {
  LockGuard guard(_lock);
  if (!_timelineStarted) return false;
  const bool late = (int32_t)(nowMs - (_timelineStartMs + dueMs)) > (int32_t)LateMs;
  const bool starving = (uint32_t)(nowMs - _lastPublishedMs) >= LateMs;
  return late && !starving;
}

void AnimationStreamWorker::fail(const char *message) {
  LockGuard guard(_lock);
  snprintf(_error, sizeof(_error), "%s", message);
  _phase = Phase::Error;
}

void *AnimationStreamWorker::gifOpen(const char *name, int32_t *size) {
  AnimationStreamWorker *worker = _current;
  if (!worker || !lockSource()) return nullptr;
  worker->_file = worker->_storage.open(name, FILE_READ, worker->_secretAccess);
  SpiBusLock::unlock();
  if (!worker->_file) return nullptr;
  *size = (int32_t)worker->_file.size();
  return &worker->_file;
}

void AnimationStreamWorker::gifClose(void *handle) {
  StorageFile *file = static_cast<StorageFile *>(handle);
  if (!file || !*file) return;
  const bool locked = SpiBusLock::lock(pdMS_TO_TICKS(SourceCloseLockMs));
  file->close();
  if (locked) SpiBusLock::unlock();
}

int32_t AnimationStreamWorker::gifRead(gif_file_tag *file, uint8_t *buffer, int32_t length) {
  if (!file || !file->fHandle || length <= 0) return 0;
  StorageFile *source = static_cast<StorageFile *>(file->fHandle);
  int32_t amount = length;
  const int32_t remaining = file->iSize - file->iPos;
  if (amount > remaining) amount = remaining;
  if (amount <= 0 || !lockSource()) return 0;
  const int read = source->read(buffer, (size_t)amount);
  SpiBusLock::unlock();
  if (read <= 0) return 0;
  file->iPos += read;
  return read;
}

int32_t AnimationStreamWorker::gifSeek(gif_file_tag *file, int32_t position) {
  if (!file || !file->fHandle || position < 0 || !lockSource()) return 0;
  StorageFile *source = static_cast<StorageFile *>(file->fHandle);
  const bool moved = source->seek((uint32_t)position);
  SpiBusLock::unlock();
  if (!moved) return file->iPos;
  file->iPos = position;
  return position;
}

bool AnimationStreamWorker::lockSource() {
  // The worker may wait for the shared SD/touch bus, but a stop request
  // abandons the transaction so the decoder returns promptly.
  for (;;) {
    AnimationStreamWorker *worker = _current;
    if (!worker || worker->_stopRequested) return false;
    if (SpiBusLock::lock(pdMS_TO_TICKS(LockRetryMs))) return true;
  }
}

void *AnimationStreamWorker::psramAlloc(uint32_t size) {
  return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void AnimationStreamWorker::psramFree(void *buffer) {
  heap_caps_free(buffer);
}
