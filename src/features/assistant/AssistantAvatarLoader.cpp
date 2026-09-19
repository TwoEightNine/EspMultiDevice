#include "AssistantAvatarLoader.h"

#include <SD.h>
#include <esp32-hal-psram.h>
#include <string.h>

#include "../../core/SpiBusLock.h"
#include "../../services/storage/SdCardController.h"

namespace {
// PNGdec expects transparent-color compositing as 00BBGGRR (RGB888), not
// the display's packed RGB565 value. Keeping this in the screen palette
// prevents transparent avatar pixels from becoming a green fringe/pane.
constexpr uint32_t BackgroundColor = 0x00000000;  // black RGB888 compositing
constexpr const char *AvatarDirectory = "/.system/ai_avatars/ch/";
// The first path is the user-facing SD path. The absolute variants keep the
// loader tolerant of the two path spellings accepted by Arduino FS builds.
constexpr const char *UnderlayPaths[] = {
    "/.system/ai_avatars/blur.png",
    "./system/ai_avatars/blur.png",
    "/system/ai_avatars/blur.png"
};
constexpr uint8_t UnderlayPathCount = sizeof(UnderlayPaths) / sizeof(UnderlayPaths[0]);

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
        const uint8_t index = (x & 1) ? (packed & 0x0f) : (packed >> 4);
        return draw.pPalette[768 + index];
      }
      if (draw.iBpp == 2) {
        const uint8_t packed = pixels[x / 4];
        const uint8_t shift = static_cast<uint8_t>(6 - ((x & 3) * 2));
        return draw.pPalette[768 + ((packed >> shift) & 0x03)];
      }
      if (draw.iBpp == 1) {
        const uint8_t packed = pixels[x / 8];
        const uint8_t index = (packed >> (7 - (x & 7))) & 0x01;
        return draw.pPalette[768 + index];
      }
      return 255;
    default:
      return 255;
  }
}
}

AssistantAvatarLoader *AssistantAvatarLoader::_decoderOwner = nullptr;

AssistantAvatarLoader::AssistantAvatarLoader(SdCardController &sdCard)
    : _sdCard(sdCard), _task(nullptr), _mutex(nullptr), _stopRequested(false),
      _running(false), _requestedGeneration(1), _revision(0), _frames{},
      _underlayFrame(nullptr), _underlayAlpha(nullptr), _underlayGeneration(0),
      _png(), _file(),
      _decodeFrame(nullptr), _decodeAlpha(nullptr), _decodingUnderlay(false) {
  memset(_frameAlpha, 0, sizeof(_frameAlpha));
  memset(_frameGeneration, 0, sizeof(_frameGeneration));
}

AssistantAvatarLoader::~AssistantAvatarLoader() {
  stop();
  (void)clearFrames();
  if (_mutex) {
    vSemaphoreDelete(_mutex);
    _mutex = nullptr;
  }
}

bool AssistantAvatarLoader::begin() {
  if (_running) return true;
  if (!_mutex) {
    _mutex = xSemaphoreCreateMutex();
    if (!_mutex) return false;
  }
  _stopRequested = false;
  _running = true;
  if (xTaskCreatePinnedToCore(taskEntry, "AssistantAvatar", 8192, this, 1,
                              &_task, 0) != pdPASS) {
    _running = false;
    _task = nullptr;
    return false;
  }
  return true;
}

void AssistantAvatarLoader::requestReload() {
  uint32_t generation = _requestedGeneration + 1;
  if (generation == 0) generation = 1;
  _requestedGeneration = generation;
  clearFrames();
}

void AssistantAvatarLoader::stop() {
  _stopRequested = true;
}

bool AssistantAvatarLoader::releaseResources() {
  if (_running) return false;
  return clearFrames();
}

bool AssistantAvatarLoader::running() const {
  return _running;
}

uint32_t AssistantAvatarLoader::revision() const {
  return _revision;
}

const uint16_t *AssistantAvatarLoader::frame(AssistantAvatar::Asset asset) const {
  const uint8_t index = static_cast<uint8_t>(asset);
  if (index >= AssetCount || !_mutex) return nullptr;
  if (xSemaphoreTake(_mutex, 0) != pdTRUE) return nullptr;
  const uint16_t *result = (_frameGeneration[index] == _requestedGeneration)
      ? _frames[index] : nullptr;
  xSemaphoreGive(_mutex);
  return result;
}

const uint8_t *AssistantAvatarLoader::frameAlpha(AssistantAvatar::Asset asset) const {
  const uint8_t index = static_cast<uint8_t>(asset);
  if (index >= AssetCount || !_mutex) return nullptr;
  if (xSemaphoreTake(_mutex, 0) != pdTRUE) return nullptr;
  const uint8_t *result = (_frameGeneration[index] == _requestedGeneration)
      ? _frameAlpha[index] : nullptr;
  xSemaphoreGive(_mutex);
  return result;
}

bool AssistantAvatarLoader::image(AssistantAvatar::Asset asset,
                                  const uint16_t *&pixels,
                                  const uint8_t *&alpha) const {
  pixels = nullptr;
  alpha = nullptr;
  const uint8_t index = static_cast<uint8_t>(asset);
  if (index >= AssetCount || !_mutex) return false;
  if (xSemaphoreTake(_mutex, 0) != pdTRUE) return false;
  if (_frameGeneration[index] == _requestedGeneration) {
    pixels = _frames[index];
    alpha = _frameAlpha[index];
  }
  const bool available = pixels != nullptr;
  xSemaphoreGive(_mutex);
  return available;
}

bool AssistantAvatarLoader::underlay(const uint16_t *&pixels,
                                     const uint8_t *&alpha) const {
  pixels = nullptr;
  alpha = nullptr;
  if (!_mutex) return false;
  if (xSemaphoreTake(_mutex, 0) != pdTRUE) return false;
  pixels = (_underlayGeneration == _requestedGeneration) ? _underlayFrame : nullptr;
  alpha = (_underlayGeneration == _requestedGeneration) ? _underlayAlpha : nullptr;
  const bool available = pixels && alpha;
  xSemaphoreGive(_mutex);
  return available;
}

void AssistantAvatarLoader::taskEntry(void *context) {
  static_cast<AssistantAvatarLoader *>(context)->run();
}

void AssistantAvatarLoader::run() {
  while (!_stopRequested) {
    const uint32_t generation = _requestedGeneration;
    bool attempted = false;
    for (uint8_t index = 0; index < AssetCount && !_stopRequested; ++index) {
      const AssistantAvatar::Asset asset = static_cast<AssistantAvatar::Asset>(index);
      if (!generationCurrent(generation)) break;
      if (!_sdCard.isMounted()) break;
      if (frame(asset)) continue;
      attempted = true;
      (void)decode(asset, generation);
    }
    const uint16_t *underlayPixels = nullptr;
    const uint8_t *underlayAlpha = nullptr;
    if (generationCurrent(generation) && !underlay(underlayPixels, underlayAlpha)) {
      attempted = true;
      (void)decodeUnderlay(generation);
    }
    if (generationCurrent(generation)) {
      vTaskDelay(pdMS_TO_TICKS(attempted ? 250 : 1000));
    } else {
      taskYIELD();
    }
  }
  _running = false;
  _task = nullptr;
  vTaskDelete(nullptr);
}

bool AssistantAvatarLoader::decode(AssistantAvatar::Asset asset,
                                   uint32_t generation) {
  if (!generationCurrent(generation) || !_sdCard.isMounted() || !psramFound()) {
    return false;
  }
  _decodingUnderlay = false;
  _decoderOwner = this;
  String path = String(AvatarDirectory) + AssistantAvatar::filename(asset);
  if (_png.open(path.c_str(), pngOpen, pngClose, pngRead, pngSeek, pngDraw) != PNG_SUCCESS) {
    _png.close();
    _decoderOwner = nullptr;
    return false;
  }
  if (_png.isInterlaced() || _png.getWidth() != Width || _png.getHeight() != Height) {
    _png.close();
    _decoderOwner = nullptr;
    return false;
  }

  const size_t pixelCount = static_cast<size_t>(Width) * Height;
  _decodeFrame = static_cast<uint16_t *>(ps_malloc(pixelCount * sizeof(uint16_t)));
  _decodeAlpha = static_cast<uint8_t *>(ps_malloc(pixelCount * sizeof(uint8_t)));
  if (!_decodeFrame || !_decodeAlpha) {
    if (_decodeFrame) free(_decodeFrame);
    if (_decodeAlpha) free(_decodeAlpha);
    _decodeFrame = nullptr;
    _decodeAlpha = nullptr;
    _png.close();
    _decoderOwner = nullptr;
    return false;
  }
  memset(_decodeFrame, 0, pixelCount * sizeof(uint16_t));
  memset(_decodeAlpha, 0, pixelCount * sizeof(uint8_t));
  const bool decoded = _png.decode(this, PNG_CHECK_CRC) == PNG_SUCCESS;
  _png.close();
  _decoderOwner = nullptr;
  uint16_t *decodedFrame = _decodeFrame;
  uint8_t *decodedAlpha = _decodeAlpha;
  _decodeFrame = nullptr;
  _decodeAlpha = nullptr;
  if (!decoded || !generationCurrent(generation)) {
    free(decodedFrame);
    free(decodedAlpha);
    return false;
  }

  const uint8_t index = static_cast<uint8_t>(asset);
  if (xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
    if (_frameGeneration[index] != _requestedGeneration) {
      if (_frames[index]) free(_frames[index]);
      if (_frameAlpha[index]) free(_frameAlpha[index]);
      _frames[index] = decodedFrame;
      _frameAlpha[index] = decodedAlpha;
      _frameGeneration[index] = generation;
      ++_revision;
      decodedFrame = nullptr;
      decodedAlpha = nullptr;
    }
    xSemaphoreGive(_mutex);
  }
  if (decodedFrame) free(decodedFrame);
  if (decodedAlpha) free(decodedAlpha);
  return true;
}

bool AssistantAvatarLoader::decodeUnderlay(uint32_t generation) {
  if (!generationCurrent(generation) || !_sdCard.isMounted() || !psramFound()) {
    return false;
  }
  _decodingUnderlay = true;
  _decoderOwner = this;
  bool opened = false;
  for (uint8_t index = 0; index < UnderlayPathCount && !opened; ++index) {
    if (_png.open(UnderlayPaths[index], pngOpen, pngClose, pngRead, pngSeek,
                  pngDraw) == PNG_SUCCESS) {
      opened = true;
    } else {
      _png.close();
    }
  }
  if (!opened) {
    _decoderOwner = nullptr;
    return false;
  }
  if (_png.isInterlaced() || _png.getWidth() != Width || _png.getHeight() != Height) {
    _png.close();
    _decoderOwner = nullptr;
    return false;
  }

  const size_t pixelCount = static_cast<size_t>(Width) * Height;
  _decodeFrame = static_cast<uint16_t *>(ps_malloc(pixelCount * sizeof(uint16_t)));
  _decodeAlpha = static_cast<uint8_t *>(ps_malloc(pixelCount * sizeof(uint8_t)));
  if (!_decodeFrame || !_decodeAlpha) {
    if (_decodeFrame) free(_decodeFrame);
    if (_decodeAlpha) free(_decodeAlpha);
    _decodeFrame = nullptr;
    _decodeAlpha = nullptr;
    _png.close();
    _decoderOwner = nullptr;
    return false;
  }
  memset(_decodeFrame, 0, pixelCount * sizeof(uint16_t));
  memset(_decodeAlpha, 0, pixelCount * sizeof(uint8_t));
  const bool decoded = _png.decode(this, PNG_CHECK_CRC) == PNG_SUCCESS;
  _png.close();
  _decoderOwner = nullptr;
  uint16_t *decodedFrame = _decodeFrame;
  uint8_t *decodedAlpha = _decodeAlpha;
  _decodeFrame = nullptr;
  _decodeAlpha = nullptr;
  if (!decoded || !generationCurrent(generation)) {
    free(decodedFrame);
    free(decodedAlpha);
    return false;
  }

  if (xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
    if (_underlayGeneration != generation || !_underlayFrame || !_underlayAlpha) {
      if (_underlayFrame) free(_underlayFrame);
      if (_underlayAlpha) free(_underlayAlpha);
      _underlayFrame = decodedFrame;
      _underlayAlpha = decodedAlpha;
      _underlayGeneration = generation;
      ++_revision;
      decodedFrame = nullptr;
      decodedAlpha = nullptr;
    }
    xSemaphoreGive(_mutex);
  }
  if (decodedFrame) free(decodedFrame);
  if (decodedAlpha) free(decodedAlpha);
  return true;
}

bool AssistantAvatarLoader::clearFrames() {
  if (!_mutex) return true;
  if (xSemaphoreTake(_mutex, _running ? 0 : portMAX_DELAY) != pdTRUE) return false;
  for (uint8_t index = 0; index < AssetCount; ++index) {
    if (_frames[index]) free(_frames[index]);
    if (_frameAlpha[index]) free(_frameAlpha[index]);
    _frames[index] = nullptr;
    _frameAlpha[index] = nullptr;
    _frameGeneration[index] = 0;
  }
  if (_underlayFrame) free(_underlayFrame);
  if (_underlayAlpha) free(_underlayAlpha);
  _underlayFrame = nullptr;
  _underlayAlpha = nullptr;
  _underlayGeneration = 0;
  xSemaphoreGive(_mutex);
  return true;
}

bool AssistantAvatarLoader::generationCurrent(uint32_t generation) const {
  return !_stopRequested && generation == _requestedGeneration;
}

void *AssistantAvatarLoader::pngOpen(const char *name, int32_t *size) {
  AssistantAvatarLoader *owner = _decoderOwner;
  if (!owner || !name || !size || !owner->_sdCard.isMounted() ||
      !SpiBusLock::lock(pdMS_TO_TICKS(100))) return nullptr;
  owner->_file = SD.open(name, FILE_READ);
  if (!owner->_file) {
    SpiBusLock::unlock();
    return nullptr;
  }
  *size = static_cast<int32_t>(owner->_file.size());
  SpiBusLock::unlock();
  return &owner->_file;
}

void AssistantAvatarLoader::pngClose(void *handle) {
  fs::File *file = static_cast<fs::File *>(handle);
  if (!file || !SpiBusLock::lock(pdMS_TO_TICKS(100))) return;
  file->close();
  SpiBusLock::unlock();
}

int32_t AssistantAvatarLoader::pngRead(PNGFILE *file, uint8_t *buffer,
                                       int32_t length) {
  if (!file || !file->fHandle || !buffer || length <= 0 ||
      !SpiBusLock::lock(pdMS_TO_TICKS(100))) return 0;
  const int32_t result = static_cast<fs::File *>(file->fHandle)->read(buffer, length);
  SpiBusLock::unlock();
  return result;
}

int32_t AssistantAvatarLoader::pngSeek(PNGFILE *file, int32_t position) {
  if (!file || !file->fHandle || position < 0 ||
      !SpiBusLock::lock(pdMS_TO_TICKS(100))) return 0;
  const bool ok = static_cast<fs::File *>(file->fHandle)->seek(position);
  SpiBusLock::unlock();
  return ok ? position : 0;
}

int AssistantAvatarLoader::pngDraw(PNGDRAW *draw) {
  AssistantAvatarLoader *owner = _decoderOwner;
  if (!owner || !draw || !owner->_decodeFrame || draw->y < 0 ||
      draw->y >= Height || draw->iWidth <= 0 || draw->iWidth > Width) return 0;
  uint16_t line[Width + 8];
  // Keep the separate alpha plane. The underlay is premultiplied against the
  // black screen background; avatar pixels stay unpremultiplied so the UI can
  // composite them over that underlay.
  owner->_png.getLineAsRGB565(
      draw, line, PNG_RGB565_LITTLE_ENDIAN,
      owner->_decodingUnderlay ? BackgroundColor : 0xffffffff);
  memcpy(owner->_decodeFrame + static_cast<size_t>(draw->y) * Width,
         line, static_cast<size_t>(draw->iWidth) * sizeof(uint16_t));
  if (owner->_decodeAlpha) {
    uint8_t *alpha = owner->_decodeAlpha + static_cast<size_t>(draw->y) * Width;
    for (int x = 0; x < draw->iWidth; ++x) alpha[x] = pixelAlpha(*draw, x);
  }
  return 1;
}
