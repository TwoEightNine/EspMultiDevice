#include "AudioCoverWorker.h"

#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

#include "../../core/SpiBusLock.h"
#include "../../media/BitmapDecode.h"
#include "../../media/FileFormats.h"

namespace {
void coverLog(const char *message, const char *path) {
  Serial.printf("[Audio cover] %s: %s\n", message, path);
}

uint16_t *allocatePsram(size_t bytes) {
  return static_cast<uint16_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}
}  // namespace

AudioCoverWorker::AudioCoverWorker(SecretStorage &secretStorage)
  : _secretStorage(secretStorage),
    _jobs(nullptr),
    _results(nullptr),
    _task(nullptr),
    _active(false),
    _wanted(0),
    _failure(nullptr) {
}

bool AudioCoverWorker::request(const String &path, bool secretAccess, uint32_t offset,
                               uint32_t length, uint32_t generation) {
  if (path.length() == 0 || path.length() >= MaxPath || generation == 0) return false;
  if (!_jobs) _jobs = xQueueCreate(1, sizeof(Job));
  if (!_results) _results = xQueueCreate(2, sizeof(Result));
  if (!_jobs || !_results) return false;
  if (!_task) {
#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
    const BaseType_t core = 0;
#else
    const BaseType_t core = ARDUINO_RUNNING_CORE == 0 ? 1 : 0;
#endif
    if (xTaskCreatePinnedToCore(taskMain, "audio-cover", 12288, this, tskIDLE_PRIORITY + 1,
                                &_task, core) != pdPASS) {
      _task = nullptr;
      return false;
    }
  }
  Job job{};
  job.generation = generation;
  job.offset = offset;
  job.length = length;
  job.secretAccess = secretAccess;
  path.toCharArray(job.path, sizeof(job.path));
  _wanted = generation;
  return xQueueOverwrite(_jobs, &job) == pdTRUE;
}

void AudioCoverWorker::cancel() {
  _wanted = 0;
  if (_jobs) {
    Job ignored{};
    while (xQueueReceive(_jobs, &ignored, 0) == pdTRUE) {}
  }
}

bool AudioCoverWorker::take(Result &result) {
  return _results && xQueueReceive(_results, &result, 0) == pdTRUE;
}

bool AudioCoverWorker::end() {
  cancel();
  if (_active) return false;
  if (_task && eTaskGetState(_task) != eBlocked) return false;
  if (_task) {
    vTaskDelete(_task);
    _task = nullptr;
  }
  if (_results) {
    Result result{};
    while (xQueueReceive(_results, &result, 0) == pdTRUE) heap_caps_free(result.pixels);
    vQueueDelete(_results);
    _results = nullptr;
  }
  if (_jobs) {
    vQueueDelete(_jobs);
    _jobs = nullptr;
  }
  return true;
}

void AudioCoverWorker::taskMain(void *context) {
  AudioCoverWorker *worker = static_cast<AudioCoverWorker *>(context);
  Job job{};
  while (true) {
    if (xQueueReceive(worker->_jobs, &job, portMAX_DELAY) != pdTRUE) continue;
    worker->_active = true;
    worker->run(job);
    worker->_active = false;
  }
}

void AudioCoverWorker::run(const Job &job) {
  if (stale(job)) return;
  _failure = nullptr;
  const uint32_t startedMs = millis();
  uint16_t *pixels = load(job);
  if (stale(job)) {
    heap_caps_free(pixels);
    coverLog("cancelled", job.path);
    return;
  }
  if (pixels) {
    Serial.printf("[Audio cover] ready in %lu ms (%lu bytes): %s\n",
                  (unsigned long)(millis() - startedMs), (unsigned long)job.length, job.path);
  } else {
    coverLog(_failure ? _failure : "failed", job.path);
  }
  Result result{job.generation, pixels};
  // The UI takes results every tick; a full queue means it has left.
  if (xQueueSend(_results, &result, 0) != pdTRUE) heap_caps_free(pixels);
}

uint16_t *AudioCoverWorker::load(const Job &job) {
  if (job.length == 0 || job.length > MaxSourceBytes) {
    _failure = "picture larger than 1 MiB";
    return nullptr;
  }
  uint8_t *source = reinterpret_cast<uint8_t *>(allocatePsram(job.length));
  if (!source) {
    _failure = "no PSRAM for the picture";
    return nullptr;
  }
  if (!readSource(job, source)) {
    heap_caps_free(source);
    return nullptr;
  }
  const FileFormats::Format format = FileFormats::fromSignature(source, job.length);
  if (format != FileFormats::Format::Jpeg && format != FileFormats::Format::Png) {
    heap_caps_free(source);
    _failure = "picture is not PNG or JPEG";
    return nullptr;
  }

  BitmapDecode::Options options;
  options.maxWidth = Size;
  options.maxHeight = Size;
  options.background = TileColor;
  options.maxSourceBytes = MaxSourceBytes;
  options.maxPixelBytes = (size_t)Size * Size * sizeof(uint16_t);
  uint16_t width = 0;
  uint16_t height = 0;
  const char *error = nullptr;
  uint16_t *decoded = BitmapDecode::decode(source, job.length, options, width, height, error);
  // The source is the largest buffer; release it before composing.
  heap_caps_free(source);
  if (!decoded) {
    // Progressive JPEGs and interlaced PNGs land here as "Unsupported image".
    _failure = error ? error : "decode failed";
    return nullptr;
  }
  if (stale(job)) {
    heap_caps_free(decoded);
    return nullptr;
  }

  uint16_t *tile = allocatePsram((size_t)Size * Size * sizeof(uint16_t));
  if (!tile) {
    heap_caps_free(decoded);
    _failure = "no PSRAM for the tile";
    return nullptr;
  }
  for (size_t i = 0; i < (size_t)Size * Size; i++) tile[i] = TileColor;
  const uint16_t left = (Size - width) / 2;
  const uint16_t top = (Size - height) / 2;
  for (uint16_t y = 0; y < height; y++) {
    memcpy(tile + (size_t)(top + y) * Size + left, decoded + (size_t)y * width,
           width * sizeof(uint16_t));
  }
  heap_caps_free(decoded);
  roundCorners(tile);
  return tile;
}

bool AudioCoverWorker::lockBus(const Job &job) {
  // Keep each wait short so cancellation is noticed quickly.
  while (!stale(job)) {
    if (SpiBusLock::lock(pdMS_TO_TICKS(50))) return true;
  }
  return false;
}

bool AudioCoverWorker::readSource(const Job &job, uint8_t *buffer) {
  if (!lockBus(job)) return false;
  StorageFile file = _secretStorage.open(job.path, FILE_READ, job.secretAccess);
  const bool inBounds = file && job.offset <= file.size() && job.length <= file.size() - job.offset;
  const bool positioned = inBounds && file.seek(job.offset);
  SpiBusLock::unlock();
  if (!positioned) {
    _failure = !file ? "cannot open the source" : "picture outside the file";
    if (file && lockBus(job)) {
      file.close();
      SpiBusLock::unlock();
    }
    return false;
  }

  bool ok = true;
  uint32_t done = 0;
  while (ok && done < job.length) {
    if (!lockBus(job)) {
      ok = false;
      break;
    }
    const uint32_t chunk = job.length - done < ChunkBytes ? job.length - done : ChunkBytes;
    ok = file.read(buffer + done, chunk) == (int)chunk;
    SpiBusLock::unlock();
    if (!ok) _failure = "read failed";
    done += chunk;
    // Leave the bus to touch polling and playback between chunks.
    vTaskDelay(1);
  }
  // Close even when the job went stale; wait as long as it takes.
  SpiBusLock::lock(portMAX_DELAY);
  file.close();
  SpiBusLock::unlock();
  return ok && !stale(job);
}

void AudioCoverWorker::roundCorners(uint16_t *pixels) {
  // Antialiased quarter circles, blended towards the black screen behind
  // the tile, matching the placeholder's fillSmoothRoundRect.
  const float radius = CornerRadius;
  for (uint16_t y = 0; y < CornerRadius; y++) {
    for (uint16_t x = 0; x < CornerRadius; x++) {
      const float dx = radius - (x + 0.5f);
      const float dy = radius - (y + 0.5f);
      float coverage = radius - sqrtf(dx * dx + dy * dy) + 0.5f;
      if (coverage >= 1.0f) continue;
      if (coverage < 0.0f) coverage = 0.0f;
      const uint16_t scale = (uint16_t)lroundf(coverage * 256.0f);
      const uint16_t corners[4][2] = {
        {x, y}, {(uint16_t)(Size - 1 - x), y},
        {x, (uint16_t)(Size - 1 - y)}, {(uint16_t)(Size - 1 - x), (uint16_t)(Size - 1 - y)}
      };
      for (const auto &corner : corners) {
        uint16_t &pixel = pixels[(size_t)corner[1] * Size + corner[0]];
        const uint16_t red = (((pixel >> 11) & 0x1f) * scale) >> 8;
        const uint16_t green = (((pixel >> 5) & 0x3f) * scale) >> 8;
        const uint16_t blue = ((pixel & 0x1f) * scale) >> 8;
        pixel = (red << 11) | (green << 5) | blue;
      }
    }
  }
}
