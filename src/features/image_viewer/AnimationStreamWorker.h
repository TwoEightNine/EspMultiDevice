#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "../../../third_party/AnimatedGIF/AnimatedGIF.h"
#include "../../services/storage/SecretStorage.h"

struct WebPAnimDecoder;

// Decodes an animated GIF or WebP directly from its source on the core the
// Arduino loop does not use and publishes display-sized RGB565 frames in
// packed PSRAM slots. While running, the worker owns the source file and the
// decoder; the UI owns presentation timing and display transfers and never
// touches either. Frames carry their due time on a timeline the UI starts when
// it shows the first frame. Every decoded frame is shown in order: when the
// decoder is slower than the authored delays, playback slows down rather than
// discarding frames it has already paid to decode.
class AnimationStreamWorker {
public:
  static constexpr uint8_t SlotCount = 3;
  static constexpr uint8_t NoSlot = 0xff;

  enum class Source : uint8_t { Gif, Webp };
  enum class Phase : uint8_t { Idle, Running, Finished, Error };

  struct Frame {
    uint8_t slot = NoSlot;
    const uint16_t *pixels = nullptr;
    uint32_t sequence = 0;
    uint32_t dueMs = 0;
  };

  struct Stats {
    Phase phase = Phase::Idle;
    uint32_t decoded = 0;
    // Frames that became ready after their due time; each restarts the
    // timeline, and lagMs totals how far playback fell behind the authored
    // timing.
    uint32_t late = 0;
    uint32_t lagMs = 0;
    uint32_t decodeAverageUs = 0;
    uint32_t decodeMaxUs = 0;
    uint32_t scaleAverageUs = 0;
    // WebP loads its whole source into PSRAM before the first frame; GIF
    // reads incrementally and reports zero.
    uint32_t sourceLoaded = 0;
    uint32_t sourceSize = 0;
  };

  explicit AnimationStreamWorker(SecretStorage &storage);
  ~AnimationStreamWorker();

  // Allocates the packed slots and starts the worker task. On failure the
  // error names the reason, including PSRAM figures for allocation failures.
  bool start(Source source, const String &path, bool secretAccess, uint32_t sourceBytes,
             uint32_t sourceWidth, uint32_t sourceHeight, uint16_t drawWidth, uint16_t drawHeight,
             String &error);
  // Blocks until the worker has released the source and decoder, then frees
  // every slot. Pixels from earlier frames must not be used afterwards.
  void stop();
  bool active() const { return _task != nullptr; }

  // Returns the next frame in order once its due time has passed; before the
  // timeline starts, the first frame. A late frame is returned at once and
  // restarts the timeline from itself. The slot stays the UI's until
  // releaseFrame().
  bool takeDueFrame(uint32_t nowMs, Frame &frame);
  void releaseFrame(uint8_t slot);
  void startTimeline(uint32_t startMs);
  Stats stats(String *error = nullptr);

private:
  enum class SlotState : uint8_t { Free, Writing, Ready, Displayed };
  enum class DecodeResult : uint8_t { Frame, LastFrame, EndOfPass, Failed };

  struct Slot {
    uint16_t *pixels;
    SlotState state;
    uint32_t sequence;
    uint32_t dueMs;
  };

  SecretStorage &_storage;
  // Allocated per GIF stream, in internal RAM when there is room, so its LZW
  // tables avoid PSRAM latency.
  AnimatedGIF *_decoder;
  Source _source;
  String _path;
  bool _secretAccess;
  StorageFile _file;
  TaskHandle_t _task;
  SemaphoreHandle_t _lock;
  SemaphoreHandle_t _stopped;
  volatile bool _stopRequested;
  bool _decoderOpen;
  uint32_t _sourceBytes;
  uint32_t _sourceWidth;
  uint32_t _sourceHeight;
  uint32_t _canvasPixels;
  uint16_t _drawWidth;
  uint16_t _drawHeight;
  uint16_t *_columnMap;
  uint16_t *_rowMap;
  // Total passes to play; zero repeats forever.
  uint32_t _passLimit;
  uint8_t *_webpData;
  uint32_t _webpLoaded;
  WebPAnimDecoder *_webpDecoder;
  const uint8_t *_webpFrame;
  uint32_t _webpPreviousTimestamp;
  Slot _slots[SlotCount];
  bool _timelineStarted;
  uint32_t _timelineStartMs;
  Phase _phase;
  char _error[96];
  uint32_t _decoded;
  uint32_t _late;
  uint32_t _lagMs;
  uint64_t _decodeTotalUs;
  uint32_t _decodeMaxUs;
  uint64_t _scaleTotalUs;
  uint32_t _scaled;
  UBaseType_t _stackHighWater;

  static AnimationStreamWorker *volatile _current;

  static void taskMain(void *context);
  void run();
  bool openGif();
  bool openWebp();
  bool loadWebpSource();
  bool prepareScaler();
  void closeDecoder();
  void decodeLoop();
  DecodeResult decodeNext(uint32_t &delayMs, int &errorCode);
  void restartPass();
  void clearGifCanvas();
  void scaleInto(uint16_t *destination);
  uint8_t acquireSlot();
  void publish(uint8_t slot, uint32_t sequence, uint32_t dueMs);
  void returnSlot(uint8_t slot);
  bool allocateGifDecoder(String &error);
  void freeGifDecoder();
  void fail(const char *message);
  void freeSlots();

  static bool lockSource();
  static void *gifOpen(const char *name, int32_t *size);
  static void gifClose(void *handle);
  static int32_t gifRead(GIFFILE *file, uint8_t *buffer, int32_t length);
  static int32_t gifSeek(GIFFILE *file, int32_t position);
  static void *psramAlloc(uint32_t size);
  static void psramFree(void *buffer);
};
