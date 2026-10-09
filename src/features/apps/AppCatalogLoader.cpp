#include "AppCatalogLoader.h"

#include <PNGdec.h>
#include <SD.h>
#include <esp32-hal-psram.h>
#include <new>
#include <string.h>

#include "AppPackage.h"
#include "../../core/SpiBusLock.h"
#include "../../services/storage/SdCardController.h"

namespace {
uint8_t pixelAlpha(const PNGDRAW &draw, int x) {
  const uint8_t *pixels = draw.pPixels;
  if (!pixels || x < 0 || x >= draw.iWidth) return 0;
  switch (draw.iPixelType) {
    case PNG_PIXEL_TRUECOLOR_ALPHA:
      return pixels[x * 4 + 3];
    case PNG_PIXEL_GRAY_ALPHA:
      return pixels[x * 2 + 1];
    case PNG_PIXEL_INDEXED:
      if (!draw.iHasAlpha || !draw.pPalette) return 255;
      if (draw.iBpp == 8) return draw.pPalette[768 + pixels[x]];
      if (draw.iBpp == 4) {
        const uint8_t packed = pixels[x / 2];
        return draw.pPalette[768 + ((x & 1) ? (packed & 0x0f) : (packed >> 4))];
      }
      if (draw.iBpp == 2) {
        const uint8_t packed = pixels[x / 4];
        const uint8_t shift = static_cast<uint8_t>(6 - ((x & 3) * 2));
        return draw.pPalette[768 + ((packed >> shift) & 0x03)];
      }
      if (draw.iBpp == 1) {
        const uint8_t packed = pixels[x / 8];
        return draw.pPalette[768 + ((packed >> (7 - (x & 7))) & 0x01)];
      }
      return 255;
    default:
      return 255;
  }
}
}

AppCatalogLoader *AppCatalogLoader::_decoderOwner = nullptr;

AppCatalogLoader::AppCatalogLoader(SdCardController &card) : _card(card) {}

AppCatalogLoader::~AppCatalogLoader() {
  stop();
  clearCatalog(_published);
  if (_png) {
    _png->~PNG();
    free(_png);
  }
  if (_mutex) vSemaphoreDelete(_mutex);
}

bool AppCatalogLoader::begin() {
  if (_running) return true;
  if (!_mutex) _mutex = xSemaphoreCreateMutex();
  if (!_mutex) return false;
  _stopRequested = false;
  _running = true;
  if (xTaskCreatePinnedToCore(taskMain, "app-catalog", 8192, this, 1,
                              &_task, 0) != pdPASS) {
    _running = false;
    _task = nullptr;
    return false;
  }
  return true;
}

void AppCatalogLoader::requestScan() {
  if (!_running && !begin()) return;
  _requestedGeneration = _requestedGeneration + 1;
  if (_task) xTaskNotifyGive(_task);
}

void AppCatalogLoader::cancel() {
  _requestedGeneration = _requestedGeneration + 1;
  if (_mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
    clearCatalog(_published);
    _publishedRevision++;
    xSemaphoreGive(_mutex);
  }
  if (_task) xTaskNotifyGive(_task);
}

void AppCatalogLoader::stop() {
  _stopRequested = true;
  if (_task) xTaskNotifyGive(_task);
}

bool AppCatalogLoader::releaseResources() {
  if (_running) return false;
  clearCatalog(_published);
  if (_png) {
    _png->~PNG();
    free(_png);
    _png = nullptr;
  }
  return true;
}

bool AppCatalogLoader::takeCatalog(Catalog &catalog, uint32_t &revision) {
  if (!_mutex || xSemaphoreTake(_mutex, 0) != pdTRUE) return false;
  if (_publishedRevision == revision) {
    xSemaphoreGive(_mutex);
    return false;
  }
  clearCatalog(catalog);
  catalog = _published;
  _published = Catalog{};
  revision = _publishedRevision;
  xSemaphoreGive(_mutex);
  return true;
}

void AppCatalogLoader::clearCatalog(Catalog &catalog) {
  for (uint8_t index = 0; index < catalog.count; ++index) {
    free(catalog.entries[index].iconColors);
    free(catalog.entries[index].iconAlpha);
    catalog.entries[index].iconColors = nullptr;
    catalog.entries[index].iconAlpha = nullptr;
  }
  catalog = Catalog{};
}

void AppCatalogLoader::taskMain(void *context) {
  static_cast<AppCatalogLoader *>(context)->run();
}

void AppCatalogLoader::run() {
  while (!_stopRequested) {
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (_stopRequested) break;
    const uint32_t generation = _requestedGeneration;
    Catalog catalog;
    scan(generation, catalog);
    if (_stopRequested || !generationCurrent(generation)) {
      clearCatalog(catalog);
      continue;
    }
    publish(generation, catalog);
    clearCatalog(catalog);
  }
  _running = false;
  _task = nullptr;
  vTaskDelete(nullptr);
}

void AppCatalogLoader::scan(uint32_t generation, Catalog &catalog) {
  if (!_card.isMounted() || !generationCurrent(generation)) return;
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return;
  File directory = SD.open("/Apps", FILE_READ);
  const bool ready = directory && directory.isDirectory();
  SpiBusLock::unlock();
  if (!ready) {
    if (directory && SpiBusLock::lock(pdMS_TO_TICKS(100))) {
      directory.close();
      SpiBusLock::unlock();
    }
    return;
  }

  uint8_t scanned = 0;
  while (scanned < MaxScanned && catalog.count < MaxApps &&
         generationCurrent(generation) && _card.isMounted()) {
    if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) {
      taskYIELD();
      continue;
    }
    File child = directory.openNextFile();
    const bool exists = static_cast<bool>(child);
    const bool isDirectory = exists && child.isDirectory();
    const String path = exists ? String(child.name()) : String();
    if (child) child.close();
    SpiBusLock::unlock();
    if (!exists) break;
    ++scanned;
    if (!isDirectory) continue;

    const int slash = path.lastIndexOf('/');
    const String id = path.substring(slash + 1);
    if (!AppPackage::validId(id)) continue;
    AppPackage package = AppPackage::load(_card, id);
    if (!package.valid || !generationCurrent(generation)) continue;

    Entry &entry = catalog.entries[catalog.count];
    id.toCharArray(entry.id, sizeof(entry.id));
    package.name.toCharArray(entry.name, sizeof(entry.name));
    entry.iconBackground = package.iconBackground;
    (void)decodeLogo(entry.id, generation, entry.iconColors, entry.iconAlpha);
    ++catalog.count;
    taskYIELD();
  }

  if (SpiBusLock::lock(pdMS_TO_TICKS(100))) {
    directory.close();
    SpiBusLock::unlock();
  }
}

bool AppCatalogLoader::decodeLogo(const char *id, uint32_t generation,
                                  uint16_t *&colors, uint8_t *&alpha) {
  colors = nullptr;
  alpha = nullptr;
  if (!id || !*id || !generationCurrent(generation) || !_card.isMounted() ||
      !psramFound()) return false;
  if (!_png) {
    void *storage = ps_malloc(sizeof(PNG));
    if (!storage) return false;
    _png = new (storage) PNG();
  }
  char path[64] = {};
  const int written = snprintf(path, sizeof(path), "/Apps/%s/logo.png", id);
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(path)) return false;

  _decoderOwner = this;
  if (_png->open(path, pngOpen, pngClose, pngRead, pngSeek, pngDraw) != PNG_SUCCESS ||
      _png->isInterlaced() || _png->getWidth() != IconWidth ||
      _png->getHeight() != IconHeight || !_png->hasAlpha()) {
    _png->close();
    _decoderOwner = nullptr;
    return false;
  }

  const size_t pixels = static_cast<size_t>(IconWidth) * IconHeight;
  _decodeColors = static_cast<uint16_t *>(ps_malloc(pixels * sizeof(uint16_t)));
  _decodeAlpha = static_cast<uint8_t *>(ps_malloc(pixels));
  if (!_decodeColors || !_decodeAlpha) {
    free(_decodeColors);
    free(_decodeAlpha);
    _decodeColors = nullptr;
    _decodeAlpha = nullptr;
    _png->close();
    _decoderOwner = nullptr;
    return false;
  }
  memset(_decodeColors, 0, pixels * sizeof(uint16_t));
  memset(_decodeAlpha, 0, pixels);
  const bool decoded = _png->decode(this, PNG_CHECK_CRC) == PNG_SUCCESS;
  _png->close();
  _decoderOwner = nullptr;
  if (!decoded || !generationCurrent(generation)) {
    free(_decodeColors);
    free(_decodeAlpha);
    _decodeColors = nullptr;
    _decodeAlpha = nullptr;
    return false;
  }
  colors = _decodeColors;
  alpha = _decodeAlpha;
  _decodeColors = nullptr;
  _decodeAlpha = nullptr;
  return true;
}

bool AppCatalogLoader::generationCurrent(uint32_t generation) const {
  return !_stopRequested && generation == _requestedGeneration;
}

void AppCatalogLoader::publish(uint32_t generation, Catalog &catalog) {
  if (!_mutex || xSemaphoreTake(_mutex, portMAX_DELAY) != pdTRUE) return;
  if (generationCurrent(generation)) {
    clearCatalog(_published);
    _published = catalog;
    catalog = Catalog{};
    _publishedRevision++;
  }
  xSemaphoreGive(_mutex);
}

void *AppCatalogLoader::pngOpen(const char *name, int32_t *size) {
  AppCatalogLoader *owner = _decoderOwner;
  if (!owner || !name || !size || !owner->_card.isMounted() ||
      !SpiBusLock::lock(pdMS_TO_TICKS(100))) return nullptr;
  owner->_pngFile = SD.open(name, FILE_READ);
  if (!owner->_pngFile) {
    SpiBusLock::unlock();
    return nullptr;
  }
  *size = static_cast<int32_t>(owner->_pngFile.size());
  SpiBusLock::unlock();
  return &owner->_pngFile;
}

void AppCatalogLoader::pngClose(void *handle) {
  fs::File *file = static_cast<fs::File *>(handle);
  if (!file || !SpiBusLock::lock(pdMS_TO_TICKS(100))) return;
  file->close();
  SpiBusLock::unlock();
}

int32_t AppCatalogLoader::pngRead(PNGFILE *file, uint8_t *buffer, int32_t length) {
  if (!file || !file->fHandle || !buffer || length <= 0 ||
      !SpiBusLock::lock(pdMS_TO_TICKS(100))) return 0;
  const int32_t result = static_cast<fs::File *>(file->fHandle)->read(buffer, length);
  SpiBusLock::unlock();
  return result;
}

int32_t AppCatalogLoader::pngSeek(PNGFILE *file, int32_t position) {
  if (!file || !file->fHandle || position < 0 ||
      !SpiBusLock::lock(pdMS_TO_TICKS(100))) return 0;
  const bool ok = static_cast<fs::File *>(file->fHandle)->seek(position);
  SpiBusLock::unlock();
  return ok ? position : 0;
}

int AppCatalogLoader::pngDraw(PNGDRAW *draw) {
  AppCatalogLoader *owner = _decoderOwner;
  if (!owner || !draw || !owner->_decodeColors || !owner->_decodeAlpha ||
      draw->y < 0 || draw->y >= IconHeight || draw->iWidth != IconWidth) return 0;
  uint16_t line[IconWidth + 8];
  owner->_png->getLineAsRGB565(draw, line, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
  const size_t offset = static_cast<size_t>(draw->y) * IconWidth;
  memcpy(owner->_decodeColors + offset, line, IconWidth * sizeof(uint16_t));
  for (int x = 0; x < IconWidth; ++x) {
    owner->_decodeAlpha[offset + x] = pixelAlpha(*draw, x);
  }
  return 1;
}
