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
  static constexpr uint16_t BackgroundWidth = 320;
  static constexpr uint16_t BackgroundHeight = 432;
  static constexpr const char *BackgroundPath = "/.system/ai_avatars/background.png";
  static constexpr uint8_t AssetCount = 5;
  static constexpr uint8_t MaxConfiguredAvatars = 9;
  static constexpr uint8_t MaxAvatars = MaxConfiguredAvatars + 1;
  static constexpr size_t AvatarNameBytes = 64;
  static constexpr size_t AvatarDirectoryBytes = 48;

  explicit AssistantAvatarLoader(SdCardController &sdCard);
  ~AssistantAvatarLoader();

  bool begin();
  void requestReload();
  void stop();
  bool releaseResources();
  bool running() const;
  uint32_t revision() const;
  uint32_t backgroundRevision() const;
  uint32_t catalogRevision() const;
  uint8_t avatarCount() const;
  String avatarName(uint8_t index) const;
  bool avatarSelected(uint8_t index) const;
  bool selectAvatar(uint8_t index);
  void resetAvatar();
  const uint16_t *frame(AssistantAvatar::Asset asset) const;
  const uint8_t *frameAlpha(AssistantAvatar::Asset asset) const;
  bool image(AssistantAvatar::Asset asset, const uint16_t *&pixels,
             const uint8_t *&alpha) const;
  bool background(const uint16_t *&pixels) const;

  // Kept public for the assistant contract diagnostic. A nonblank config line
  // must have exactly one usable Name:dir pair.
  static bool parseConfigLine(const char *line, char *name, size_t nameBytes,
                              char *directory, size_t directoryBytes);
  static bool buildAvatarPath(const char *directory, AssistantAvatar::Asset asset,
                              char *path, size_t pathBytes);

private:
  static void taskEntry(void *context);
  void run();
  bool loadCatalog();
  bool decode(AssistantAvatar::Asset asset, uint32_t generation);
  bool decodeBackground(uint32_t generation);
  bool clearFrames();
  bool generationCurrent(uint32_t generation) const;
  bool directoryForGeneration(uint32_t generation, char *directory,
                              size_t directoryBytes) const;
  void advanceGeneration();
  void wake();

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
  volatile bool _catalogReloadRequested;
  volatile uint32_t _catalogRevision;
  char _avatarNames[MaxAvatars][AvatarNameBytes];
  char _avatarDirectories[MaxAvatars][AvatarDirectoryBytes];
  uint8_t _avatarCount;
  char _selectedDirectory[AvatarDirectoryBytes];
  uint16_t *_frames[AssetCount];
  uint8_t *_frameAlpha[AssetCount];
  uint32_t _frameGeneration[AssetCount];
  uint16_t *_backgroundFrame;
  uint32_t _backgroundGeneration;
  uint32_t _backgroundAttemptGeneration;
  volatile uint32_t _backgroundRevision;
  PNG _png;
  fs::File _file;
  uint16_t *_decodeFrame;
  uint8_t *_decodeAlpha;
  bool _decodingBackground;
  uint16_t _decodeWidth;
  uint16_t _decodeHeight;
  static AssistantAvatarLoader *_decoderOwner;
};
