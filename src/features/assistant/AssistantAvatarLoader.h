#pragma once

#include <Arduino.h>
#include <FS.h>
#include <PNGdec.h>

#include "AssistantAvatar.h"

class SdCardController;

// Screen-scoped SD/PNG worker. It never draws; the UI task consumes the
// published RGB565 frames through frame().
class AssistantAvatarLoader {
public:
  static constexpr uint16_t Width = 320;
  static constexpr uint16_t Height = 240;
  static constexpr uint8_t AssetCount = 5;

  explicit AssistantAvatarLoader(SdCardController &sdCard);
  ~AssistantAvatarLoader();

  bool begin();
  void requestReload();
  void stop();
  bool releaseResources();
  bool running() const;
  uint32_t revision() const;
  const uint16_t *frame(AssistantAvatar::Asset asset) const;
  const uint8_t *frameAlpha(AssistantAvatar::Asset asset) const;
  bool image(AssistantAvatar::Asset asset, const uint16_t *&pixels,
             const uint8_t *&alpha) const;
  bool underlay(const uint16_t *&pixels, const uint8_t *&alpha) const;

private:
  static void taskEntry(void *context);
  void run();
  bool decode(AssistantAvatar::Asset asset, uint32_t generation);
  bool decodeUnderlay(uint32_t generation);
  bool clearFrames();
  bool generationCurrent(uint32_t generation) const;

  static void *pngOpen(const char *name, int32_t *size);
  static void pngClose(void *handle);
  static int32_t pngRead(PNGFILE *file, uint8_t *buffer, int32_t length);
  static int32_t pngSeek(PNGFILE *file, int32_t position);
  static int pngDraw(PNGDRAW *draw);

  SdCardController &_sdCard;
  TaskHandle_t _task;
  SemaphoreHandle_t _mutex;
  volatile bool _stopRequested;
  volatile bool _running;
  volatile uint32_t _requestedGeneration;
  volatile uint32_t _revision;
  uint16_t *_frames[AssetCount];
  uint8_t *_frameAlpha[AssetCount];
  uint32_t _frameGeneration[AssetCount];
  uint16_t *_underlayFrame;
  uint8_t *_underlayAlpha;
  uint32_t _underlayGeneration;
  PNG _png;
  fs::File _file;
  uint16_t *_decodeFrame;
  uint8_t *_decodeAlpha;
  bool _decodingUnderlay;
  static AssistantAvatarLoader *_decoderOwner;
};
