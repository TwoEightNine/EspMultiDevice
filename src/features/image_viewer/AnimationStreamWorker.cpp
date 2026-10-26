#include "AnimationStreamWorker.h"

#include "../../../third_party/libwebp/src/webp/decode.h"
#include "../../../third_party/libwebp/src/webp/demux.h"
#include "../../core/SpiBusLock.h"

#include <esp32-hal-psram.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <new>
#include <stdio.h>
#include <string.h>

namespace {
constexpr uint32_t WorkerStackBytes = 8192;
constexpr size_t SlotAlignment = 64;
// Lateness up to this is absorbed by the timeline (touch polling, a DMA
// transfer still in flight); beyond it the timeline restarts at the frame.
constexpr uint32_t LateToleranceMs = 20;
// Internal RAM left free after placing the GIF decoder state there.
constexpr size_t InternalReserveBytes = 32768;
// PSRAM a resident first pass must leave free for the rest of the system.
constexpr uint32_t ReplayReserveBytes = 512UL * 1024UL;
constexpr uint16_t MaxResidentFrames = 512;
constexpr uint32_t LockRetryMs = 20;
constexpr uint32_t SourceCloseLockMs = 250;
// WebP sources load through an internal-RAM bounce buffer, one bounded SD
// transaction per chunk. Smaller chunks only cost more transactions, so a
// short internal heap degrades speed rather than failing the open.
constexpr uint32_t SourceChunkSizes[] = {8192, 2048, 512};
// Delays of 10 ms or less play at 100 ms, as browsers do. AnimatedGIF already
// applies this to explicit GIF delays; GIF frames without a graphic control
// extension and zero-duration WebP frames report zero.
constexpr uint32_t ShortDelayMs = 10;
constexpr uint32_t ShortDelayReplacementMs = 100;

// Hot per-pixel tables prefer internal RAM but work from PSRAM.
void *allocPreferInternal(size_t bytes) {
  void *memory = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return memory ? memory : heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// Bookkeeping that can be large but is touched rarely lives in PSRAM, so
// internal RAM stays available for buffers that need it.
void *callocPreferPsram(size_t count, size_t bytes) {
  void *memory = heap_caps_calloc(count, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return memory ? memory : heap_caps_calloc(count, bytes, MALLOC_CAP_8BIT);
}

class LockGuard {
public:
  explicit LockGuard(SemaphoreHandle_t lock) : _lock(lock) { xSemaphoreTake(_lock, portMAX_DELAY); }
  ~LockGuard() { xSemaphoreGive(_lock); }

private:
  SemaphoreHandle_t _lock;
};
}  // namespace

AnimationStreamWorker *volatile AnimationStreamWorker::_current = nullptr;

AnimationStreamWorker::AnimationStreamWorker(SecretStorage &storage)
    : _storage(storage), _decoder(nullptr), _source(Source::Gif), _secretAccess(false), _task(nullptr),
      _lock(nullptr), _stopped(nullptr), _stopRequested(false), _decoderOpen(false), _sourceBytes(0),
      _sourceWidth(0), _sourceHeight(0), _canvasPixels(0), _drawWidth(0), _drawHeight(0),
      _columnMap(nullptr), _rowMap(nullptr), _passLimit(1), _webpData(nullptr), _webpLoaded(0),
      _webpDecoder(nullptr), _webpFrame(nullptr), _webpPreviousTimestamp(0), _slots(nullptr),
      _slotCapacity(0), _allocatedSlots(0), _slotBytes(0), _frameSlots(nullptr), _expectedFrames(0),
      _mode(Mode::Stream), _residentFrames(0), _passDurationMs(0), _nextSequence(0),
      _timelineStarted(false), _timelineStartMs(0),
      _phase(Phase::Idle), _error{}, _decoded(0), _late(0), _lagMs(0),
      _decodeTotalUs(0), _decodeMaxUs(0), _scaleTotalUs(0), _scaled(0), _stackHighWater(0) {}

AnimationStreamWorker::~AnimationStreamWorker() {
  stop();
  if (_lock) vSemaphoreDelete(_lock);
}

bool AnimationStreamWorker::start(Source source, const String &path, bool secretAccess,
                                  uint32_t sourceBytes, uint32_t sourceWidth, uint32_t sourceHeight,
                                  uint16_t drawWidth, uint16_t drawHeight, String &error) {
  stop();
  if (!_lock) _lock = xSemaphoreCreateMutex();
  _stopped = xSemaphoreCreateBinary();
  if (!_lock || !_stopped) {
    error = F("Animation worker setup failed");
    stop();
    return false;
  }
  if (!sourceWidth || !sourceHeight || !drawWidth || !drawHeight ||
      (source == Source::Webp && !sourceBytes)) {
    error = F("Invalid animation size");
    stop();
    return false;
  }
  const uint64_t canvasPixels = (uint64_t)sourceWidth * sourceHeight;
  // AnimatedGIF's cooked canvas keeps indexed, RGB565, and previous-frame
  // copies plus a spare row: five bytes per canvas pixel and three per column.
  // WebP keeps the whole source plus libwebp's current and previous RGBA
  // canvases.
  const uint64_t decoderBytes = source == Source::Gif
    ? canvasPixels * 5ULL + (uint64_t)sourceWidth * 3ULL
    : (uint64_t)sourceBytes + canvasPixels * 8ULL;
  const uint64_t largestBlock = source == Source::Gif
    ? decoderBytes
    : (canvasPixels * 4ULL > sourceBytes ? canvasPixels * 4ULL : (uint64_t)sourceBytes);
  const uint32_t slotBytes = (uint32_t)drawWidth * drawHeight * sizeof(uint16_t);
  const uint32_t freePsram = ESP.getFreePsram();
  const uint32_t largestPsram = ESP.getMaxAllocPsram();
  if (decoderBytes + (uint64_t)slotBytes * StreamSlots > freePsram || largestBlock > largestPsram) {
    error = String(F("Animation needs too much PSRAM (")) +
            String((uint32_t)((decoderBytes + (uint64_t)slotBytes * StreamSlots) / 1024ULL)) +
            F(" KB; free ") + String(freePsram / 1024) + F(" KB, largest ") +
            String(largestPsram / 1024) + F(" KB)");
    stop();
    return false;
  }
  // Room for a whole first pass beside the decoder, the stream ring, and the
  // reserve. The progressive check while recording is authoritative; this
  // only sizes the slot table.
  const uint64_t spare = freePsram > decoderBytes + ReplayReserveBytes
    ? freePsram - decoderBytes - ReplayReserveBytes : 0;
  uint64_t capacity = spare / slotBytes;
  if (capacity > MaxResidentFrames) capacity = MaxResidentFrames;
  if (capacity < StreamSlots) capacity = StreamSlots;
  _slotCapacity = (uint16_t)capacity;
  _slotBytes = slotBytes;
  _slots = static_cast<Slot *>(callocPreferPsram(_slotCapacity, sizeof(Slot)));
  _frameSlots = static_cast<uint16_t *>(callocPreferPsram(_slotCapacity, sizeof(uint16_t)));
  bool allocated = _slots && _frameSlots;
  for (uint16_t index = 0; allocated && index < StreamSlots; ++index) {
    allocated = allocateSlotBuffer(_slots[index]);
  }
  if (!allocated) {
    error = String(F("Animation frames need ")) + String((slotBytes * StreamSlots) / 1024) +
            F(" KB of PSRAM; largest ") + String(ESP.getMaxAllocPsram() / 1024) + F(" KB");
    stop();
    return false;
  }
  if (source == Source::Gif && !allocateGifDecoder(error)) {
    stop();
    return false;
  }
  _source = source;
  _path = path;
  _secretAccess = secretAccess;
  _sourceBytes = sourceBytes;
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
  freeGifDecoder();
  _path = "";
  _stopRequested = false;
  _timelineStarted = false;
  _timelineStartMs = 0;
  _phase = Phase::Idle;
  _error[0] = '\0';
  _decoded = 0;
  _late = 0;
  _lagMs = 0;
  _decodeTotalUs = 0;
  _decodeMaxUs = 0;
  _scaleTotalUs = 0;
  _scaled = 0;
  _stackHighWater = 0;
  _webpLoaded = 0;
  _sourceBytes = 0;
  _expectedFrames = 0;
  _mode = Mode::Stream;
  _residentFrames = 0;
  _passDurationMs = 0;
  _nextSequence = 0;
}

bool AnimationStreamWorker::allocateGifDecoder(String &error) {
  // The decoder's LZW tables and line buffers are hit for every pixel. Keep
  // them in internal RAM unless that would leave too little for the rest of
  // the system; PSRAM still works, just more slowly.
  const size_t bytes = sizeof(AnimatedGIF);
  void *memory = nullptr;
  const size_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t internalLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (internalLargest >= bytes && internalFree >= bytes + InternalReserveBytes) {
    memory = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  const bool internal = memory != nullptr;
  if (!memory) memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!memory) {
    error = F("GIF decoder needs more memory");
    return false;
  }
  _decoder = new (memory) AnimatedGIF();
  // Leaves the frame-buffer pointer null for closeDecoder().
  _decoder->begin(GIF_PALETTE_RGB565_LE);
  Serial.printf("[Anim] GIF decoder state %u bytes in %s RAM (internal free %u, largest %u, "
                "reserve %u)\n", (unsigned)bytes, internal ? "internal" : "PSRAM", (unsigned)internalFree,
                (unsigned)internalLargest, (unsigned)InternalReserveBytes);
  return true;
}

void AnimationStreamWorker::freeGifDecoder() {
  if (!_decoder) return;
  _decoder->~AnimatedGIF();
  heap_caps_free(_decoder);
  _decoder = nullptr;
}

bool AnimationStreamWorker::allocateSlotBuffer(Slot &slot) {
  slot = {};
  slot.pixels = static_cast<uint16_t *>(
      heap_caps_aligned_alloc(SlotAlignment, _slotBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!slot.pixels) return false;
  slot.state = SlotState::Free;
  ++_allocatedSlots;
  return true;
}

void AnimationStreamWorker::freeSlots() {
  if (_slots) {
    for (uint16_t index = 0; index < _slotCapacity; ++index) {
      if (_slots[index].pixels) heap_caps_free(_slots[index].pixels);
    }
    heap_caps_free(_slots);
  }
  if (_frameSlots) heap_caps_free(_frameSlots);
  _slots = nullptr;
  _frameSlots = nullptr;
  _slotCapacity = 0;
  _allocatedSlots = 0;
  _slotBytes = 0;
}

bool AnimationStreamWorker::takeDueFrame(uint32_t nowMs, Frame &frame) {
  if (!_task || !_lock || !_slots) return false;
  LockGuard guard(_lock);
  if (_mode == Mode::Replay) return takeReplayFrameLocked(nowMs, frame);
  uint16_t chosen = NoSlot;
  for (uint16_t index = 0; index < _slotCapacity; ++index) {
    const Slot &slot = _slots[index];
    if (slot.state != SlotState::Ready) continue;
    if (chosen == NoSlot || slot.sequence < _slots[chosen].sequence) chosen = index;
  }
  if (chosen == NoSlot) return false;
  Slot &slot = _slots[chosen];
  if (!admitLocked(nowMs, slot.dueMs)) return false;
  slot.state = SlotState::Displayed;
  frame.slot = chosen;
  frame.pixels = slot.pixels;
  frame.sequence = slot.sequence;
  frame.dueMs = slot.dueMs;
  _nextSequence = slot.sequence + 1;
  return true;
}

bool AnimationStreamWorker::admitLocked(uint32_t nowMs, uint32_t dueMs) {
  if (!_timelineStarted) return true;
  const int32_t lateness = (int32_t)(nowMs - (_timelineStartMs + dueMs));
  if (lateness < 0) return false;
  // A decoder slower than the authored delays makes every frame late. Show
  // it now and measure the following delays from here, so playback slows
  // instead of skipping, and never fast-forwards to catch up.
  if (lateness > (int32_t)LateToleranceMs) {
    _timelineStartMs = nowMs - dueMs;
    ++_late;
    _lagMs += (uint32_t)lateness;
  }
  return true;
}

bool AnimationStreamWorker::takeReplayFrameLocked(uint32_t nowMs, Frame &frame) {
  if (_residentFrames == 0) return false;
  const uint32_t sequence = _nextSequence;
  const uint32_t pass = sequence / _residentFrames;
  if (_passLimit != 0 && pass >= _passLimit) {
    // The last frame of the final pass stays on screen.
    if (_phase == Phase::Running) _phase = Phase::Finished;
    return false;
  }
  const uint16_t index = _frameSlots[sequence % _residentFrames];
  const uint32_t dueMs = pass * _passDurationMs + _slots[index].dueMs;
  if (!admitLocked(nowMs, dueMs)) return false;
  frame.slot = index;
  frame.pixels = _slots[index].pixels;
  frame.sequence = sequence;
  frame.dueMs = dueMs;
  ++_nextSequence;
  return true;
}

void AnimationStreamWorker::releaseFrame(uint16_t slot) {
  if (!_lock || !_slots || slot >= _slotCapacity) return;
  {
    LockGuard guard(_lock);
    // Resident frames stay put during replay.
    if (_mode == Mode::Replay) return;
    Slot &released = _slots[slot];
    if (released.state == SlotState::Displayed) {
      if (_mode == Mode::Record && released.resident) {
        released.state = SlotState::Kept;
      } else if (_mode == Mode::Stream && _allocatedSlots > StreamSlots) {
        // Buffers left over from an abandoned recording go back to PSRAM once
        // shown; streaming needs only its ring.
        heap_caps_free(released.pixels);
        released = {};
        --_allocatedSlots;
      } else {
        released.state = SlotState::Free;
      }
    }
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
  copy.late = _late;
  copy.lagMs = _lagMs;
  copy.decodeAverageUs = _decoded ? (uint32_t)(_decodeTotalUs / _decoded) : 0;
  copy.decodeMaxUs = _decodeMaxUs;
  copy.scaleAverageUs = _scaled ? (uint32_t)(_scaleTotalUs / _scaled) : 0;
  if (_source == Source::Webp) {
    copy.sourceLoaded = _webpLoaded;
    copy.sourceSize = _sourceBytes;
  }
  copy.mode = _mode;
  copy.slots = _allocatedSlots;
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
  const bool opened = _source == Source::Gif ? openGif() : openWebp();
  if (opened) {
    // Keep the first pass only when it can be replayed: the animation loops,
    // and PSRAM could plausibly hold every frame.
    LockGuard guard(_lock);
    if (_passLimit != 1 && _slotCapacity > StreamSlots &&
        (_expectedFrames == 0 || _expectedFrames <= _slotCapacity)) {
      _mode = Mode::Record;
    }
  }
  if (opened && prepareScaler()) decodeLoop();
  // Release the source and decoder as soon as playback ends or fails; the
  // UI keeps showing its slot until it stops the worker.
  closeDecoder();
  while (!_stopRequested) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
  _stackHighWater = uxTaskGetStackHighWaterMark(nullptr);
  _current = nullptr;
  xSemaphoreGive(_stopped);
  vTaskDelete(nullptr);
}

bool AnimationStreamWorker::openGif() {
  _decoder->begin(GIF_PALETTE_RGB565_LE);
  const int opened = _decoder->open(_path.c_str(), gifOpen, gifClose, gifRead, gifSeek, nullptr);
  if (!opened) {
    if (!_stopRequested) {
      char message[48];
      snprintf(message, sizeof(message), "GIF header error %d", _decoder->getLastError());
      fail(message);
    }
    if (_file) gifClose(&_file);
    return false;
  }
  _decoderOpen = true;
  if ((uint32_t)_decoder->getCanvasWidth() != _sourceWidth ||
      (uint32_t)_decoder->getCanvasHeight() != _sourceHeight) {
    fail("GIF header changed while opening");
    return false;
  }
  _canvasPixels = _sourceWidth * _sourceHeight;
  // A null draw callback makes AnimatedGIF keep the complete cooked RGB565
  // canvas, including disposal and transparency, in its frame buffer.
  _decoder->setDrawType(GIF_DRAW_COOKED);
  if (_decoder->allocFrameBuf(psramAlloc) != GIF_SUCCESS) {
    char message[96];
    snprintf(message, sizeof(message), "GIF canvas needs too much PSRAM (largest %u KB)",
             (unsigned)(ESP.getMaxAllocPsram() / 1024));
    fail(message);
    return false;
  }
  clearGifCanvas();
  // NETSCAPE loop count: none plays once, zero repeats forever, and N
  // repeats N more times.
  const int loopCount = _decoder->getLoopCount();
  _passLimit = loopCount < 0 ? 1 : loopCount == 0 ? 0 : (uint32_t)loopCount + 1;
  return true;
}

bool AnimationStreamWorker::loadWebpSource() {
  if (!lockSource()) return false;
  _file = _storage.open(_path, FILE_READ, _secretAccess);
  const uint32_t size = _file ? (uint32_t)_file.size() : 0;
  SpiBusLock::unlock();
  if (!_file || size != _sourceBytes) {
    fail(_file ? "WebP source changed while opening" : "Unable to open WebP source");
    return false;
  }
  _webpData = static_cast<uint8_t *>(psramAlloc(size));
  if (!_webpData) {
    char message[96];
    snprintf(message, sizeof(message), "WebP source needs too much PSRAM (%u KB; largest %u KB)",
             (unsigned)(size / 1024), (unsigned)(ESP.getMaxAllocPsram() / 1024));
    fail(message);
    return false;
  }
  uint8_t *chunk = nullptr;
  uint32_t chunkBytes = 0;
  for (const uint32_t candidate : SourceChunkSizes) {
    chunk = static_cast<uint8_t *>(heap_caps_malloc(candidate, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (chunk) {
      chunkBytes = candidate;
      break;
    }
  }
  if (!chunk) {
    char message[96];
    snprintf(message, sizeof(message), "WebP loader needs internal RAM (free %u, largest %u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    fail(message);
    return false;
  }
  uint32_t loaded = 0;
  while (loaded < size) {
    const uint32_t amount = size - loaded > chunkBytes ? chunkBytes : size - loaded;
    if (!lockSource()) break;
    const int read = _file.read(chunk, amount);
    SpiBusLock::unlock();
    if (read != (int)amount) {
      fail("WebP source read failed");
      break;
    }
    memcpy(_webpData + loaded, chunk, amount);
    loaded += amount;
    {
      LockGuard guard(_lock);
      _webpLoaded = loaded;
    }
    // Leave the shared SD/touch bus free for a moment between chunks.
    vTaskDelay(1);
  }
  heap_caps_free(chunk);
  gifClose(&_file);
  return loaded == size;
}

bool AnimationStreamWorker::openWebp() {
  if (!loadWebpSource()) return false;
  WebPAnimDecoderOptions options;
  if (!WebPAnimDecoderOptionsInit(&options)) {
    fail("WebP animation setup failed");
    return false;
  }
  // Premultiplied output composes transparency over the black canvas.
  options.color_mode = MODE_rgbA;
  options.use_threads = 0;
  // The decoder keeps pointing into this data until it is deleted.
  WebPData data = {_webpData, _sourceBytes};
  _webpDecoder = WebPAnimDecoderNew(&data, &options);
  WebPAnimInfo info;
  if (!_webpDecoder || !WebPAnimDecoderGetInfo(_webpDecoder, &info) || info.frame_count == 0) {
    char message[96];
    snprintf(message, sizeof(message), "Invalid WebP animation or too little PSRAM (largest %u KB)",
             (unsigned)(ESP.getMaxAllocPsram() / 1024));
    fail(message);
    return false;
  }
  if (info.canvas_width != _sourceWidth || info.canvas_height != _sourceHeight) {
    fail("WebP header changed while opening");
    return false;
  }
  // WebP loop count is the total number of passes; zero repeats forever.
  _passLimit = info.loop_count;
  _expectedFrames = info.frame_count;
  _webpPreviousTimestamp = 0;
  return true;
}

bool AnimationStreamWorker::prepareScaler() {
  _columnMap = static_cast<uint16_t *>(allocPreferInternal((size_t)_drawWidth * sizeof(uint16_t)));
  _rowMap = static_cast<uint16_t *>(allocPreferInternal((size_t)_drawHeight * sizeof(uint16_t)));
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
    _decoder->close();
    _decoderOpen = false;
  }
  if (_decoder && _decoder->getFrameBuf()) _decoder->freeFrameBuf(psramFree);
  if (_webpDecoder) WebPAnimDecoderDelete(_webpDecoder);
  _webpDecoder = nullptr;
  _webpFrame = nullptr;
  if (_webpData) psramFree(_webpData);
  _webpData = nullptr;
  if (_file) gifClose(&_file);
  if (_columnMap) heap_caps_free(_columnMap);
  if (_rowMap) heap_caps_free(_rowMap);
  _columnMap = nullptr;
  _rowMap = nullptr;
  freeGifDecoder();
}

void AnimationStreamWorker::clearGifCanvas() {
  uint8_t *canvas = _decoder->getFrameBuf();
  if (canvas) memset(canvas, 0, (size_t)_canvasPixels * 5 + (size_t)_sourceWidth * 3);
}

void AnimationStreamWorker::decodeLoop() {
  uint32_t passes = 0;
  uint32_t passFrames = 0;
  uint32_t sequence = 0;
  uint32_t dueMs = 0;
  bool reportedTruncation = false;
  while (!_stopRequested) {
    uint16_t slot = acquireSlot();
    while (slot == NoSlot && !_stopRequested) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
      slot = acquireSlot();
    }
    if (_stopRequested) {
      if (slot != NoSlot) returnSlot(slot);
      return;
    }

    uint32_t delayMs = 0;
    int errorCode = 0;
    const int64_t decodeStarted = esp_timer_get_time();
    const DecodeResult result = decodeNext(delayMs, errorCode);
    const uint32_t decodeUs = (uint32_t)(esp_timer_get_time() - decodeStarted);
    if (_stopRequested) {
      returnSlot(slot);
      return;
    }
    const bool frameDecoded = result == DecodeResult::Frame || result == DecodeResult::LastFrame;
    const bool endOfPass = result != DecodeResult::Frame;
    if (result == DecodeResult::Failed) {
      if (passFrames == 0) {
        returnSlot(slot);
        char message[48];
        if (_source == Source::Gif) snprintf(message, sizeof(message), "GIF decode error %d", errorCode);
        else snprintf(message, sizeof(message), "WebP animation decode failed");
        fail(message);
        return;
      }
      // A truncated or damaged file loops the frames that did decode, as
      // browsers do.
      if (!reportedTruncation) {
        Serial.printf("[Anim] stream ended early after %lu frame(s), error %d\n",
                      (unsigned long)passFrames, errorCode);
        reportedTruncation = true;
      }
    }

    const bool finalPass = _passLimit != 0 && passes + 1 >= _passLimit;
    // A single-frame source never needs a second decode.
    const bool finishing = endOfPass && (finalPass || (passFrames + (frameDecoded ? 1 : 0)) <= 1);
    if (frameDecoded) {
      {
        LockGuard guard(_lock);
        ++_decoded;
        _decodeTotalUs += decodeUs;
        if (decodeUs > _decodeMaxUs) _decodeMaxUs = decodeUs;
      }
      // Decoding is the expensive part and cannot be skipped, because later
      // frames build on this canvas; scaling and showing it is cheap, so
      // every decoded frame is published.
      const int64_t scaleStarted = esp_timer_get_time();
      scaleInto(_slots[slot].pixels);
      const uint32_t scaleUs = (uint32_t)(esp_timer_get_time() - scaleStarted);
      {
        LockGuard guard(_lock);
        _scaleTotalUs += scaleUs;
        ++_scaled;
      }
      publish(slot, sequence, dueMs, passes == 0 ? (int32_t)passFrames : -1);
      ++sequence;
      ++passFrames;
      dueMs += delayMs <= ShortDelayMs ? ShortDelayReplacementMs : delayMs;
    } else {
      returnSlot(slot);
    }

    if (endOfPass) {
      // One summary per stream sizes what a full pass would need in memory.
      if (passes == 0) {
        Serial.printf("[Anim] pass 1: %lu frame(s), %lu ms as authored, canvas %lux%lu, "
                      "frame %lu bytes\n", (unsigned long)passFrames, (unsigned long)dueMs,
                      (unsigned long)_sourceWidth, (unsigned long)_sourceHeight,
                      (unsigned long)((uint32_t)_drawWidth * _drawHeight * sizeof(uint16_t)));
      }
      if (finishing) {
        LockGuard guard(_lock);
        if (_phase == Phase::Running) _phase = Phase::Finished;
        return;
      }
      if (passes == 0 && _mode == Mode::Record) {
        // Every first-pass frame is resident; run() now frees the decoder.
        beginReplay((uint16_t)passFrames, dueMs);
        return;
      }
      ++passes;
      passFrames = 0;
      restartPass();
    }
    // Let the idle task run between frames on this core.
    vTaskDelay(1);
  }
}

AnimationStreamWorker::DecodeResult AnimationStreamWorker::decodeNext(uint32_t &delayMs,
                                                                      int &errorCode) {
  if (_source == Source::Gif) {
    int delay = 0;
    const int result = _decoder->playFrame(false, &delay, nullptr);
    errorCode = _decoder->getLastError();
    if (result < 0 || (errorCode != GIF_SUCCESS && errorCode != GIF_EMPTY_FRAME)) {
      return DecodeResult::Failed;
    }
    if (errorCode == GIF_EMPTY_FRAME) return DecodeResult::EndOfPass;
    delayMs = delay > 0 ? (uint32_t)delay : 0;
    return result == 0 ? DecodeResult::LastFrame : DecodeResult::Frame;
  }
  if (!WebPAnimDecoderHasMoreFrames(_webpDecoder)) return DecodeResult::EndOfPass;
  uint8_t *rgba = nullptr;
  int timestamp = 0;
  if (!WebPAnimDecoderGetNext(_webpDecoder, &rgba, &timestamp) || !rgba) {
    _webpFrame = nullptr;
    return DecodeResult::Failed;
  }
  _webpFrame = rgba;
  // A WebP timestamp is the end time of its frame, so the difference from the
  // previous one is this frame's duration.
  const uint32_t current = timestamp > 0 ? (uint32_t)timestamp : 0;
  delayMs = current >= _webpPreviousTimestamp ? current - _webpPreviousTimestamp : 0;
  _webpPreviousTimestamp = current;
  return WebPAnimDecoderHasMoreFrames(_webpDecoder) ? DecodeResult::Frame : DecodeResult::LastFrame;
}

void AnimationStreamWorker::restartPass() {
  if (_source == Source::Gif) {
    _decoder->reset();
    // Each pass starts from an empty canvas, like the first.
    clearGifCanvas();
    return;
  }
  // libwebp clears its canvas when it decodes the first frame again.
  WebPAnimDecoderReset(_webpDecoder);
  _webpPreviousTimestamp = 0;
  _webpFrame = nullptr;
}

void AnimationStreamWorker::scaleInto(uint16_t *destination) {
  if (_source == Source::Webp) {
    const uint32_t pitch = _sourceWidth * 4;
    for (uint32_t y = 0; y < _drawHeight; ++y) {
      const uint8_t *row = _webpFrame + (size_t)_rowMap[y] * pitch;
      uint16_t *out = destination + (size_t)y * _drawWidth;
      for (uint32_t x = 0; x < _drawWidth; ++x) {
        const uint8_t *pixel = row + (size_t)_columnMap[x] * 4;
        out[x] = (uint16_t)(((pixel[0] & 0xf8) << 8) | ((pixel[1] & 0xfc) << 3) | (pixel[2] >> 3));
      }
    }
    return;
  }
  const uint8_t *cooked = _decoder->getFrameBuf() + _canvasPixels;
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

uint16_t AnimationStreamWorker::acquireSlot() {
  LockGuard guard(_lock);
  for (uint16_t index = 0; index < _slotCapacity; ++index) {
    if (_slots[index].state == SlotState::Free && _slots[index].pixels) {
      _slots[index].state = SlotState::Writing;
      return index;
    }
  }
  if (_mode != Mode::Record) return NoSlot;
  // While recording, each first-pass frame keeps its own buffer.
  if (_allocatedSlots < _slotCapacity && ESP.getMaxAllocPsram() >= _slotBytes &&
      ESP.getFreePsram() >= _slotBytes + ReplayReserveBytes) {
    for (uint16_t index = 0; index < _slotCapacity; ++index) {
      if (_slots[index].pixels) continue;
      if (!allocateSlotBuffer(_slots[index])) break;
      _slots[index].state = SlotState::Writing;
      return index;
    }
  }
  abandonRecordingLocked();
  for (uint16_t index = 0; index < _slotCapacity; ++index) {
    if (_slots[index].state == SlotState::Free && _slots[index].pixels) {
      _slots[index].state = SlotState::Writing;
      return index;
    }
  }
  return NoSlot;
}

void AnimationStreamWorker::abandonRecordingLocked() {
  _mode = Mode::Stream;
  uint16_t released = 0;
  for (uint16_t index = 0; index < _slotCapacity; ++index) {
    Slot &slot = _slots[index];
    slot.resident = false;
    if (slot.state != SlotState::Kept) continue;
    heap_caps_free(slot.pixels);
    slot = {};
    --_allocatedSlots;
    ++released;
  }
  Serial.printf("[Anim] first pass does not fit in PSRAM; streaming every loop (released %u "
                "frame(s), free %lu)\n", (unsigned)released, (unsigned long)ESP.getFreePsram());
}

void AnimationStreamWorker::beginReplay(uint16_t frames, uint32_t durationMs) {
  uint32_t residentBytes = 0;
  {
    LockGuard guard(_lock);
    // Buffers that never held a first-pass frame are not needed for replay.
    for (uint16_t index = 0; index < _slotCapacity; ++index) {
      Slot &slot = _slots[index];
      if (!slot.pixels || slot.resident) continue;
      heap_caps_free(slot.pixels);
      slot = {};
      --_allocatedSlots;
    }
    _mode = Mode::Replay;
    _residentFrames = frames;
    _passDurationMs = durationMs;
    residentBytes = (uint32_t)_allocatedSlots * _slotBytes;
  }
  Serial.printf("[Anim] replaying %u resident frame(s) (%lu KB); decoder released\n", (unsigned)frames,
                (unsigned long)(residentBytes / 1024));
}

void AnimationStreamWorker::publish(uint16_t slot, uint32_t sequence, uint32_t dueMs,
                                    int32_t firstPassFrame) {
  LockGuard guard(_lock);
  _slots[slot].sequence = sequence;
  _slots[slot].dueMs = dueMs;
  _slots[slot].state = SlotState::Ready;
  if (_mode == Mode::Record && firstPassFrame >= 0) {
    _slots[slot].resident = true;
    _frameSlots[firstPassFrame] = slot;
  }
}

void AnimationStreamWorker::returnSlot(uint16_t slot) {
  LockGuard guard(_lock);
  if (_slots[slot].state == SlotState::Writing) _slots[slot].state = SlotState::Free;
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

int32_t AnimationStreamWorker::gifRead(GIFFILE *file, uint8_t *buffer, int32_t length) {
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

int32_t AnimationStreamWorker::gifSeek(GIFFILE *file, int32_t position) {
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
