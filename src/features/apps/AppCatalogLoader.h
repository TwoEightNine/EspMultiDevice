#pragma once

#include <Arduino.h>
#include <FS.h>

class PNG;
typedef struct png_file_tag PNGFILE;
typedef struct png_draw_tag PNGDRAW;

class SdCardController;

// Home-owned worker that discovers runnable SD apps and decodes their small
// icon assets. The worker publishes complete catalogs; only the UI task draws.
class AppCatalogLoader {
public:
  static constexpr uint8_t MaxApps = 32;
  static constexpr uint8_t MaxScanned = 128;
  static constexpr uint16_t IconWidth = 64;
  static constexpr uint16_t IconHeight = 64;

  struct Entry {
    char id[33] = {};
    char name[41] = {};
    uint16_t iconBackground = 0;
    uint16_t *iconColors = nullptr;
    uint8_t *iconAlpha = nullptr;
  };

  struct Catalog {
    Entry entries[MaxApps];
    uint8_t count = 0;
  };

  explicit AppCatalogLoader(SdCardController &card);
  ~AppCatalogLoader();

  bool begin();
  void requestScan();
  void cancel();
  void stop();
  bool releaseResources();
  bool takeCatalog(Catalog &catalog, uint32_t &revision);

  static void clearCatalog(Catalog &catalog);

private:
  static void taskMain(void *context);
  void run();
  void scan(uint32_t generation, Catalog &catalog);
  bool decodeLogo(const char *id, uint32_t generation,
                  uint16_t *&colors, uint8_t *&alpha);
  bool generationCurrent(uint32_t generation) const;
  void publish(uint32_t generation, Catalog &catalog);

  static void *pngOpen(const char *name, int32_t *size);
  static void pngClose(void *handle);
  static int32_t pngRead(PNGFILE *file, uint8_t *buffer, int32_t length);
  static int32_t pngSeek(PNGFILE *file, int32_t position);
  static int pngDraw(PNGDRAW *draw);

  SdCardController &_card;
  TaskHandle_t _task = nullptr;
  SemaphoreHandle_t _mutex = nullptr;
  volatile bool _running = false;
  volatile bool _stopRequested = false;
  volatile uint32_t _requestedGeneration = 0;
  uint32_t _publishedRevision = 0;
  Catalog _published;

  PNG *_png = nullptr;
  fs::File _pngFile;
  uint16_t *_decodeColors = nullptr;
  uint8_t *_decodeAlpha = nullptr;
  static AppCatalogLoader *_decoderOwner;
};
