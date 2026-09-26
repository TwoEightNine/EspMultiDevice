#include "AssistantAvatarLoader.h"

#include <SD.h>
#include <esp32-hal-psram.h>
#include <stdio.h>
#include <string.h>

#include "../../core/SpiBusLock.h"
#include "../../services/storage/SdCardController.h"

namespace {
// PNGdec expects transparent-color compositing as 00BBGGRR (RGB888), not
// the display's packed RGB565 value. Keeping this in the screen palette
// prevents transparent avatar pixels from becoming a green fringe/pane.
constexpr uint32_t BackgroundColor = 0x00000000;  // black RGB888 compositing
constexpr const char *AvatarRoot = "/.system/ai_avatars/";
constexpr const char *AvatarConfigPath = "/.system/ai_avatars/avatars.config";
constexpr const char *DefaultAvatarName = "Default";
constexpr const char *DefaultAvatarDirectory = "default";
constexpr size_t MaxAvatarConfigBytes = 2048;
constexpr size_t ReadChunkBytes = 128;

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

bool isDirectoryCharacter(char value) {
  return (value >= 'a' && value <= 'z') ||
         (value >= 'A' && value <= 'Z') ||
         (value >= '0' && value <= '9') || value == '_' || value == '-';
}

bool copyTrimmed(const char *begin, const char *end, char *destination,
                 size_t destinationBytes) {
  if (!begin || !end || end < begin || !destination || destinationBytes < 2) return false;
  while (begin < end && (*begin == ' ' || *begin == '\t')) begin++;
  while (end > begin && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
  const size_t length = static_cast<size_t>(end - begin);
  if (length == 0 || length >= destinationBytes) return false;
  for (size_t index = 0; index < length; ++index) {
    const unsigned char character = static_cast<unsigned char>(begin[index]);
    if (character < 0x20 || character == 0x7f) return false;
    destination[index] = begin[index];
  }
  destination[length] = 0;
  return true;
}
}

AssistantAvatarLoader *AssistantAvatarLoader::_decoderOwner = nullptr;

AssistantAvatarLoader::AssistantAvatarLoader(SdCardController &sdCard)
    : _sdCard(sdCard), _task(nullptr), _mutex(nullptr), _stopRequested(false),
      _running(false), _requestedGeneration(1), _revision(0), _frames{},
      _catalogReloadRequested(true), _catalogRevision(0), _avatarNames{},
      _avatarDirectories{}, _avatarCount(1), _selectedDirectory{},
      _backgroundFrame(nullptr), _backgroundGeneration(0),
      _backgroundAttemptGeneration(0), _backgroundRevision(0),
      _png(), _file(),
      _decodeFrame(nullptr), _decodeAlpha(nullptr), _decodingBackground(false),
      _decodeWidth(Width), _decodeHeight(Height) {
  memset(_frameAlpha, 0, sizeof(_frameAlpha));
  memset(_frameGeneration, 0, sizeof(_frameGeneration));
  strncpy(_avatarNames[0], DefaultAvatarName, AvatarNameBytes - 1);
  strncpy(_avatarDirectories[0], DefaultAvatarDirectory, AvatarDirectoryBytes - 1);
  strncpy(_selectedDirectory, DefaultAvatarDirectory, AvatarDirectoryBytes - 1);
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
  _catalogReloadRequested = true;
  advanceGeneration();
  clearFrames();
  wake();
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

uint32_t AssistantAvatarLoader::backgroundRevision() const {
  return _backgroundRevision;
}

uint32_t AssistantAvatarLoader::catalogRevision() const {
  return _catalogRevision;
}

uint8_t AssistantAvatarLoader::avatarCount() const {
  if (!_mutex || xSemaphoreTake(_mutex, 0) != pdTRUE) return 1;
  const uint8_t result = _avatarCount;
  xSemaphoreGive(_mutex);
  return result;
}

String AssistantAvatarLoader::avatarName(uint8_t index) const {
  if (!_mutex || xSemaphoreTake(_mutex, 0) != pdTRUE) return index == 0 ? String(DefaultAvatarName) : String();
  const String result = index < _avatarCount ? String(_avatarNames[index]) : String();
  xSemaphoreGive(_mutex);
  return result;
}

bool AssistantAvatarLoader::avatarSelected(uint8_t index) const {
  if (!_mutex || xSemaphoreTake(_mutex, 0) != pdTRUE) return index == 0;
  const bool result = index < _avatarCount &&
      strcmp(_selectedDirectory, _avatarDirectories[index]) == 0;
  xSemaphoreGive(_mutex);
  return result;
}

bool AssistantAvatarLoader::selectAvatar(uint8_t index) {
  if (!_mutex || xSemaphoreTake(_mutex, 0) != pdTRUE) return false;
  if (index >= _avatarCount) {
    xSemaphoreGive(_mutex);
    return false;
  }
  const bool changed = strcmp(_selectedDirectory, _avatarDirectories[index]) != 0;
  if (changed) {
    strncpy(_selectedDirectory, _avatarDirectories[index], AvatarDirectoryBytes - 1);
    _selectedDirectory[AvatarDirectoryBytes - 1] = 0;
  }
  xSemaphoreGive(_mutex);
  if (changed) {
    advanceGeneration();
    wake();
  }
  return true;
}

void AssistantAvatarLoader::resetAvatar() {
  if (!_mutex || xSemaphoreTake(_mutex, 0) != pdTRUE) return;
  const bool changed = strcmp(_selectedDirectory, DefaultAvatarDirectory) != 0;
  if (changed) {
    strncpy(_selectedDirectory, DefaultAvatarDirectory, AvatarDirectoryBytes - 1);
    _selectedDirectory[AvatarDirectoryBytes - 1] = 0;
  }
  xSemaphoreGive(_mutex);
  if (changed) {
    advanceGeneration();
    wake();
  }
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
  // A selection change retains the previous frame until the replacement is
  // decoded, so changing avatars never blanks the pane or the dialogue above.
  pixels = _frames[index];
  alpha = _frameAlpha[index];
  const bool available = pixels != nullptr;
  xSemaphoreGive(_mutex);
  return available;
}

bool AssistantAvatarLoader::background(const uint16_t *&pixels) const {
  pixels = nullptr;
  if (!_mutex) return false;
  if (xSemaphoreTake(_mutex, 0) != pdTRUE) return false;
  pixels = (_backgroundGeneration == _requestedGeneration) ? _backgroundFrame : nullptr;
  const bool available = pixels != nullptr;
  xSemaphoreGive(_mutex);
  return available;
}

void AssistantAvatarLoader::taskEntry(void *context) {
  static_cast<AssistantAvatarLoader *>(context)->run();
}

void AssistantAvatarLoader::run() {
  while (!_stopRequested) {
    if (_catalogReloadRequested) (void)loadCatalog();
    const uint32_t generation = _requestedGeneration;
    bool attempted = false;
    const uint16_t *backgroundPixels = nullptr;
    if (generationCurrent(generation) && generation != _backgroundAttemptGeneration &&
        !background(backgroundPixels)) {
      attempted = true;
      _backgroundAttemptGeneration = generation;
      (void)decodeBackground(generation);
    }
    for (uint8_t index = 0; index < AssetCount && !_stopRequested; ++index) {
      const AssistantAvatar::Asset asset = static_cast<AssistantAvatar::Asset>(index);
      if (!generationCurrent(generation)) break;
      if (!_sdCard.isMounted()) break;
      if (frame(asset)) continue;
      attempted = true;
      (void)decode(asset, generation);
    }
    if (generationCurrent(generation)) {
      (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(attempted ? 250 : 1000));
    } else {
      taskYIELD();
    }
  }
  _running = false;
  _task = nullptr;
  vTaskDelete(nullptr);
}

bool AssistantAvatarLoader::loadCatalog() {
  _catalogReloadRequested = false;
  char config[MaxAvatarConfigBytes + 1] = {};
  size_t length = 0;
  bool valid = _sdCard.isMounted();
  fs::File file;
  if (valid) {
    if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) valid = false;
    if (valid) {
      file = SD.open(AvatarConfigPath, FILE_READ);
      if (!file || file.size() > MaxAvatarConfigBytes) valid = false;
      SpiBusLock::unlock();
    }
  }
  while (valid && length < MaxAvatarConfigBytes) {
    const size_t request = min(ReadChunkBytes, MaxAvatarConfigBytes - length);
    if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) { valid = false; break; }
    const size_t read = file.readBytes(config + length, request);
    SpiBusLock::unlock();
    if (read == 0) break;
    length += read;
  }
  if (file) {
    if (SpiBusLock::lock(pdMS_TO_TICKS(100))) {
      file.close();
      SpiBusLock::unlock();
    } else {
      valid = false;
    }
  }
  if (valid && length == MaxAvatarConfigBytes) valid = false;

  char names[MaxAvatars][AvatarNameBytes] = {};
  char directories[MaxAvatars][AvatarDirectoryBytes] = {};
  uint8_t count = 1;
  strncpy(names[0], DefaultAvatarName, AvatarNameBytes - 1);
  strncpy(directories[0], DefaultAvatarDirectory, AvatarDirectoryBytes - 1);
  if (valid) {
    size_t lineStart = 0;
    for (size_t cursor = 0; cursor <= length; ++cursor) {
      if (cursor != length && config[cursor] != '\n') continue;
      char *line = config + lineStart;
      config[cursor] = 0;
      while (*line == ' ' || *line == '\t' || *line == '\r') line++;
      if (*line) {
        char parsedName[AvatarNameBytes] = {};
        char parsedDirectory[AvatarDirectoryBytes] = {};
        if (!parseConfigLine(line, parsedName, sizeof(parsedName), parsedDirectory,
                             sizeof(parsedDirectory))) {
          valid = false;
          break;
        }
        if (count < MaxAvatars) {
          strncpy(names[count], parsedName, AvatarNameBytes - 1);
          strncpy(directories[count], parsedDirectory, AvatarDirectoryBytes - 1);
          count++;
        }
      }
      lineStart = cursor + 1;
    }
  }
  if (!valid) count = 1;
  if (!_mutex || xSemaphoreTake(_mutex, portMAX_DELAY) != pdTRUE) return false;
  memcpy(_avatarNames, names, sizeof(_avatarNames));
  memcpy(_avatarDirectories, directories, sizeof(_avatarDirectories));
  _avatarCount = count;
  bool selectedAvailable = false;
  for (uint8_t index = 0; index < count; ++index) {
    if (!strcmp(_selectedDirectory, _avatarDirectories[index])) {
      selectedAvailable = true;
      break;
    }
  }
  if (!selectedAvailable) {
    strncpy(_selectedDirectory, DefaultAvatarDirectory, AvatarDirectoryBytes - 1);
    _selectedDirectory[AvatarDirectoryBytes - 1] = 0;
    advanceGeneration();
  }
  _catalogRevision = _catalogRevision + 1;
  xSemaphoreGive(_mutex);
  return valid;
}

bool AssistantAvatarLoader::parseConfigLine(const char *line, char *name,
                                            size_t nameBytes, char *directory,
                                            size_t directoryBytes) {
  if (!line || !name || !directory) return false;
  const char *separator = strchr(line, ':');
  if (!separator || strchr(separator + 1, ':')) return false;
  const char *end = line + strlen(line);
  if (!copyTrimmed(line, separator, name, nameBytes) ||
      !copyTrimmed(separator + 1, end, directory, directoryBytes)) return false;
  for (const char *character = directory; *character; ++character) {
    if (!isDirectoryCharacter(*character)) return false;
  }
  return true;
}

bool AssistantAvatarLoader::buildAvatarPath(const char *directory,
                                            AssistantAvatar::Asset asset,
                                            char *path, size_t pathBytes) {
  if (!directory || !*directory || !path || pathBytes == 0) return false;
  for (const char *character = directory; *character; ++character) {
    if (!isDirectoryCharacter(*character)) return false;
  }
  const int written = snprintf(path, pathBytes, "%s%s/%s", AvatarRoot, directory,
                               AssistantAvatar::filename(asset));
  return written > 0 && static_cast<size_t>(written) < pathBytes;
}

bool AssistantAvatarLoader::decode(AssistantAvatar::Asset asset,
                                   uint32_t generation) {
  if (!generationCurrent(generation) || !_sdCard.isMounted() || !psramFound()) {
    return false;
  }
  _decodingBackground = false;
  _decodeWidth = Width;
  _decodeHeight = Height;
  _decoderOwner = this;
  char directory[AvatarDirectoryBytes] = {};
  char path[AvatarDirectoryBytes + 48] = {};
  if (!directoryForGeneration(generation, directory, sizeof(directory)) ||
      !buildAvatarPath(directory, asset, path, sizeof(path)) ||
      _png.open(path, pngOpen, pngClose, pngRead, pngSeek, pngDraw) != PNG_SUCCESS) {
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

bool AssistantAvatarLoader::decodeBackground(uint32_t generation) {
  if (!generationCurrent(generation) || !_sdCard.isMounted() || !psramFound()) {
    return false;
  }
  _decodingBackground = true;
  _decodeWidth = BackgroundWidth;
  _decodeHeight = BackgroundHeight;
  _decoderOwner = this;
  if (_png.open(BackgroundPath, pngOpen, pngClose, pngRead, pngSeek, pngDraw) !=
      PNG_SUCCESS) {
    _png.close();
    _decoderOwner = nullptr;
    _decodingBackground = false;
    return false;
  }
  if (_png.isInterlaced() || _png.getWidth() != BackgroundWidth ||
      _png.getHeight() != BackgroundHeight) {
    _png.close();
    _decoderOwner = nullptr;
    _decodingBackground = false;
    return false;
  }

  const size_t pixelCount = static_cast<size_t>(BackgroundWidth) * BackgroundHeight;
  _decodeFrame = static_cast<uint16_t *>(ps_malloc(pixelCount * sizeof(uint16_t)));
  _decodeAlpha = nullptr;
  if (!_decodeFrame) {
    _png.close();
    _decoderOwner = nullptr;
    _decodingBackground = false;
    return false;
  }
  const bool decoded = _png.decode(this, PNG_CHECK_CRC) == PNG_SUCCESS;
  _png.close();
  _decoderOwner = nullptr;
  _decodingBackground = false;
  uint16_t *decodedFrame = _decodeFrame;
  _decodeFrame = nullptr;
  if (!decoded || !generationCurrent(generation)) {
    free(decodedFrame);
    return false;
  }

  if (xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
    if (_backgroundGeneration != generation || !_backgroundFrame) {
      if (_backgroundFrame) free(_backgroundFrame);
      _backgroundFrame = decodedFrame;
      _backgroundGeneration = generation;
      ++_backgroundRevision;
      ++_revision;
      decodedFrame = nullptr;
    }
    xSemaphoreGive(_mutex);
  }
  if (decodedFrame) free(decodedFrame);
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
  if (_backgroundFrame) free(_backgroundFrame);
  _backgroundFrame = nullptr;
  _backgroundGeneration = 0;
  _backgroundAttemptGeneration = 0;
  ++_backgroundRevision;
  xSemaphoreGive(_mutex);
  return true;
}

bool AssistantAvatarLoader::generationCurrent(uint32_t generation) const {
  return !_stopRequested && generation == _requestedGeneration;
}

bool AssistantAvatarLoader::directoryForGeneration(uint32_t generation,
                                                    char *directory,
                                                    size_t directoryBytes) const {
  if (!directory || directoryBytes == 0 || !_mutex ||
      xSemaphoreTake(_mutex, 0) != pdTRUE) return false;
  const bool current = generation == _requestedGeneration;
  if (current) {
    strncpy(directory, _selectedDirectory, directoryBytes - 1);
    directory[directoryBytes - 1] = 0;
  }
  xSemaphoreGive(_mutex);
  return current;
}

void AssistantAvatarLoader::advanceGeneration() {
  uint32_t generation = _requestedGeneration + 1;
  if (generation == 0) generation = 1;
  _requestedGeneration = generation;
}

void AssistantAvatarLoader::wake() {
  if (_task) xTaskNotifyGive(_task);
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
      draw->y >= owner->_decodeHeight || draw->iWidth <= 0 ||
      draw->iWidth > owner->_decodeWidth) return 0;
  uint16_t line[Width + 8];
  // Avatar pixels keep a separate alpha plane; background transparency is
  // flattened against black before its RGB565 frame is published.
  owner->_png.getLineAsRGB565(
      draw, line, PNG_RGB565_LITTLE_ENDIAN,
      owner->_decodingBackground ? BackgroundColor : 0xffffffff);
  memcpy(owner->_decodeFrame + static_cast<size_t>(draw->y) * owner->_decodeWidth,
         line, static_cast<size_t>(draw->iWidth) * sizeof(uint16_t));
  if (owner->_decodeAlpha) {
    uint8_t *alpha = owner->_decodeAlpha +
        static_cast<size_t>(draw->y) * owner->_decodeWidth;
    for (int x = 0; x < draw->iWidth; ++x) alpha[x] = pixelAlpha(*draw, x);
  }
  return 1;
}
