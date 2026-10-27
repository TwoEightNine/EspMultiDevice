#include "DownloadStore.h"

#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../../core/SpiBusLock.h"
#include "SdCardController.h"

DownloadStore::DownloadStore(SdCardController &sdCard, const String &directory, uint32_t maxBytes)
    : _sdCard(sdCard),
      _directory(directory),
      _maxBytes(maxBytes),
      _format(FileFormats::Format::Unknown),
      _byteLimit(0),
      _bytesWritten(0),
      _writeElapsedMs(0),
      _active(false) {}

DownloadStore::~DownloadStore() {
  abort();
}

bool DownloadStore::ensureDirectory() {
  if (SD.exists(_directory.c_str())) return true;
  int16_t slash = _directory.indexOf('/', 1);
  while (slash > 0) {
    const String level = _directory.substring(0, slash);
    if (!SD.exists(level.c_str()) && !SD.mkdir(level.c_str())) return false;
    slash = _directory.indexOf('/', slash + 1);
  }
  return SD.mkdir(_directory.c_str());
}

bool DownloadStore::begin(FileFormats::Format format, const String &name,
                          int32_t declaredSize, uint32_t byteLimit, String &error) {
  error = "";
  if (_active) {
    error = "Download already active";
    return false;
  }
  reset();
  if (!FileFormats::isDownloadable(format)) {
    error = "Unsupported file download";
    return false;
  }
  if (!_sdCard.isMounted()) {
    error = "SD card required for downloads";
    return false;
  }

  if (byteLimit == 0 || byteLimit > _maxBytes) byteLimit = _maxBytes;
  if (declaredSize > 0 && static_cast<uint32_t>(declaredSize) > byteLimit) {
    error = "File exceeds download limit";
    return false;
  }
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) {
    error = "SD card is busy";
    return false;
  }

  if (!ensureDirectory()) {
    SpiBusLock::unlock();
    error = "Cannot create Downloads directory";
    return false;
  }

  const String extension = FileFormats::extension(format);
  const uint32_t stamp = millis();
  for (uint16_t attempt = 0; attempt < MaxNameAttempts; ++attempt) {
    String candidate = name;
    if (!candidate.length()) candidate = String(F("media-")) + String(static_cast<unsigned long>(stamp)) + extension;
    if (attempt) {
      const int dot = candidate.lastIndexOf('.');
      const String suffix = dot > 0 ? candidate.substring(dot) : extension;
      const String stem = dot > 0 ? candidate.substring(0, dot) : candidate;
      candidate = stem + "-" + String(attempt) + suffix;
    }
    const String finalPath = _directory + "/" + candidate;
    const String temporaryPath = finalPath + F(".tmp");
    if (SD.exists(finalPath.c_str())) continue;
    if (SD.exists(temporaryPath.c_str()) && !SD.remove(temporaryPath.c_str())) continue;
    fs::File file = SD.open(temporaryPath.c_str(), FILE_WRITE);
    if (!file) {
      if (SD.exists(temporaryPath.c_str())) SD.remove(temporaryPath.c_str());
      continue;
    }
    _file = file;
    _finalPath = finalPath;
    _temporaryPath = temporaryPath;
    break;
  }
  SpiBusLock::unlock();

  if (!_file) {
    reset();
    error = "Cannot create download file";
    return false;
  }
  _format = format;
  _byteLimit = byteLimit;
  _bytesWritten = 0;
  _writeElapsedMs = 0;
  _active = true;
  return true;
}

bool DownloadStore::write(const uint8_t *bytes, size_t length, String &error) {
  error = "";
  if (!_active || !_file) {
    error = "Download not started";
    return false;
  }
  if (length == 0) return true;
  if (!bytes) {
    error = "File download write failed";
    return false;
  }
  if (length > static_cast<size_t>(_byteLimit - _bytesWritten)) {
    error = "File exceeds download limit";
    return false;
  }

  size_t offset = 0;
  while (offset < length) {
    const size_t remaining = length - offset;
    const size_t amount = remaining > WriteChunkBytes ? WriteChunkBytes : remaining;
    if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) {
      error = "SD card is busy";
      return false;
    }
    const uint32_t writeStartedMs = millis();
    const size_t written = _file.write(bytes + offset, amount);
    _writeElapsedMs += millis() - writeStartedMs;
    SpiBusLock::unlock();
    if (written != amount) {
      error = "File download write failed";
      return false;
    }
    offset += amount;
    _bytesWritten += static_cast<uint32_t>(amount);
    yield();
  }
  return true;
}

bool DownloadStore::commit(String &finalPath, String &error) {
  finalPath = "";
  error = "";
  if (!_active || !_file) {
    error = "Download not started";
    return false;
  }
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) {
    error = "SD card is busy";
    abort();
    return false;
  }
  _file.close();
  SpiBusLock::unlock();

  if (!validateTemporaryFile()) {
    error = "Unsupported file download";
    abort();
    return false;
  }
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) {
    error = "SD card is busy";
    abort();
    return false;
  }
  const bool committed = !SD.exists(_finalPath.c_str()) &&
                         SD.rename(_temporaryPath.c_str(), _finalPath.c_str());
  if (!committed) SD.remove(_temporaryPath.c_str());
  SpiBusLock::unlock();
  if (!committed) {
    error = "Cannot save file download";
    reset();
    return false;
  }

  finalPath = _finalPath;
  reset();
  return true;
}

void DownloadStore::abort() {
  if (!_file && !_temporaryPath.length()) {
    reset();
    return;
  }
  SpiBusLock::lock();
  if (_file) _file.close();
  if (_temporaryPath.length()) SD.remove(_temporaryPath.c_str());
  SpiBusLock::unlock();
  reset();
}

bool DownloadStore::discard(const String &path) {
  if (!_sdCard.isMounted() || !path.startsWith(_directory + "/") ||
      path.endsWith(F(".tmp")) ||
      !SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
  const bool removed = !SD.exists(path.c_str()) || SD.remove(path.c_str());
  SpiBusLock::unlock();
  return removed;
}

bool DownloadStore::validateTemporaryFile() {
  if (!_temporaryPath.length() || !SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
  fs::File file = SD.open(_temporaryPath.c_str(), FILE_READ);
  uint8_t signature[32] = {};
  const int bytes = file ? file.read(signature, sizeof(signature)) : 0;
  if (file) file.close();
  SpiBusLock::unlock();

  const FileFormats::Format actual =
      FileFormats::fromSignature(signature, bytes > 0 ? static_cast<size_t>(bytes) : 0);
  if (actual == _format) return true;
  // Some legitimate 3GP writers use a generic ISO-BMFF major brand. Permit
  // that ambiguity, but never accept identified 3GP when MP4 was declared.
  return _format == FileFormats::Format::ThreeGp && actual == FileFormats::Format::Mp4;
}

void DownloadStore::reset() {
  _file = fs::File();
  _format = FileFormats::Format::Unknown;
  _finalPath = "";
  _temporaryPath = "";
  _byteLimit = 0;
  _bytesWritten = 0;
  _writeElapsedMs = 0;
  _active = false;
}
