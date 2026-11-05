#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "../../services/storage/SecretStorage.h"

// Screen-scoped worker that turns an embedded audio cover into the player's
// 208x208 artwork tile. It runs on the core opposite the UI loop, reads the
// picture bytes in 8 KiB chunks (each under SpiBusLock), classifies them with
// FileFormats, decodes PNG or baseline JPEG through BitmapDecode, and fits the
// result with its aspect kept, centred on the tile colour, with antialiased
// rounded corners blended to black. It never draws or navigates.
//
// Jobs and results carry the screen's generation number. cancel() or a newer
// request makes the running job stale; the worker stops at its next chunk,
// frees its own buffers, and publishes nothing. A published result's pixels
// belong to the receiver, which frees them with heap_caps_free.
class AudioCoverWorker {
public:
  static constexpr uint16_t Size = 208;
  static constexpr uint16_t CornerRadius = 12;
  static constexpr uint16_t TileColor = 0x2104;  // #222222
  static constexpr uint32_t MaxSourceBytes = 1024UL * 1024UL;

  struct Result {
    uint32_t generation;
    // Size x Size RGB565 in PSRAM, or null when the cover cannot be shown.
    uint16_t *pixels;
  };

  explicit AudioCoverWorker(SecretStorage &secretStorage);

  // Starts the task on first use and queues the job, replacing a queued one.
  bool request(const String &path, bool secretAccess, uint32_t offset, uint32_t length,
               uint32_t generation);
  // Makes every job stale and drops a queued one.
  void cancel();
  bool take(Result &result);
  // Stops the task once it is idle. Returns false while a job is still
  // unwinding; call again from a later tick.
  bool end();

private:
  static const uint16_t MaxPath = 256;
  static const uint32_t ChunkBytes = 8192;

  struct Job {
    uint32_t generation;
    uint32_t offset;
    uint32_t length;
    bool secretAccess;
    char path[MaxPath];
  };

  SecretStorage &_secretStorage;
  QueueHandle_t _jobs;
  QueueHandle_t _results;
  TaskHandle_t _task;
  volatile bool _active;
  volatile uint32_t _wanted;
  const char *_failure;

  static void taskMain(void *context);
  void run(const Job &job);
  uint16_t *load(const Job &job);
  bool readSource(const Job &job, uint8_t *buffer);
  bool lockBus(const Job &job);
  bool stale(const Job &job) const { return job.generation != _wanted; }
  static void roundCorners(uint16_t *pixels);
};
