#include "BrowserDownloadStore.h"

#include <SD.h>
#include <ctype.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "BrowserLimits.h"
#include "../../core/SpiBusLock.h"
#include "../../services/storage/SdCardController.h"

namespace {
constexpr const char *DownloadDirectory = "/Downloads";

String trimCopy(const String &value) {
  String result = value;
  result.trim();
  return result;
}

String lowerCopy(const String &value) {
  String result = value;
  result.toLowerCase();
  return result;
}
}  // namespace

BrowserDownloadStore::BrowserDownloadStore(SdCardController &sdCard)
    : _sdCard(sdCard),
      _format(FileFormats::Format::Unknown),
      _byteLimit(0),
      _bytesWritten(0),
      _active(false) {}

BrowserDownloadStore::~BrowserDownloadStore() {
  abort();
}

FileFormats::Format BrowserDownloadStore::classify(const String &contentType,
                                                   const String &urlPath) {
  String type = lowerCopy(trimCopy(contentType));
  const int semicolon = type.indexOf(';');
  if (semicolon >= 0) type.remove(semicolon);
  type.trim();

  FileFormats::Format format = FileFormats::fromContentType(type);
  if (FileFormats::isDownloadable(format)) return format;
  if (type.length() == 0 || type == F("application/octet-stream")) {
    format = FileFormats::fromPath(urlPath);
    if (FileFormats::isDownloadable(format)) return format;
  }
  return FileFormats::Format::Unknown;
}

String BrowserDownloadStore::deriveSuggestedName(const String &contentDisposition,
                                                  const String &urlPath,
                                                  FileFormats::Format format) {
  String candidate;
  const String lowerDisposition = lowerCopy(contentDisposition);
  int position = lowerDisposition.indexOf(F("filename*="));
  if (position >= 0) {
    candidate = trimCopy(contentDisposition.substring(position + 10));
    const int separator = candidate.indexOf(';');
    if (separator >= 0) candidate.remove(separator);
    if (candidate.length() >= 2 && candidate[0] == '"' && candidate[candidate.length() - 1] == '"') {
      candidate = candidate.substring(1, candidate.length() - 1);
    }
    const int charsetEnd = candidate.indexOf('\'');
    const int languageEnd = charsetEnd < 0 ? -1 : candidate.indexOf('\'', charsetEnd + 1);
    if (languageEnd >= 0) candidate = candidate.substring(languageEnd + 1);
  }
  if (!candidate.length()) {
    position = lowerDisposition.indexOf(F("filename="));
    if (position >= 0) {
      candidate = trimCopy(contentDisposition.substring(position + 9));
      const int separator = candidate.indexOf(';');
      if (separator >= 0) candidate.remove(separator);
      if (candidate.length() >= 2 && candidate[0] == '"' && candidate[candidate.length() - 1] == '"') {
        candidate = candidate.substring(1, candidate.length() - 1);
      }
    }
  }
  if (!candidate.length()) {
    candidate = urlPath;
    const int query = candidate.indexOf('?');
    if (query >= 0) candidate.remove(query);
    const int slash = candidate.lastIndexOf('/');
    if (slash >= 0) candidate = candidate.substring(slash + 1);
  }

  String decoded;
  for (uint16_t index = 0; index < candidate.length(); ++index) {
    if (candidate[index] == '%' && index + 2 < candidate.length() &&
        isxdigit(static_cast<unsigned char>(candidate[index + 1])) &&
        isxdigit(static_cast<unsigned char>(candidate[index + 2]))) {
      char encoded[] = {candidate[index + 1], candidate[index + 2], '\0'};
      decoded += static_cast<char>(strtoul(encoded, nullptr, 16));
      index += 2;
    } else {
      decoded += candidate[index];
    }
  }

  String safe;
  for (uint16_t index = 0; index < decoded.length() && safe.length() < MaxNameBytes; ++index) {
    const uint8_t character = static_cast<uint8_t>(decoded[index]);
    if (character < 0x20 || character == 0x7f || character == '/' || character == '\\' ||
        character == ':' || character == '*' || character == '?' || character == '"' ||
        character == '<' || character == '>' || character == '|') continue;
    safe += static_cast<char>(character);
  }
  safe.trim();
  while (safe.endsWith(F("."))) safe.remove(safe.length() - 1);
  if (!safe.length() || safe == F(".") || safe == F("..")) return "";

  if (FileFormats::fromPath(safe) != format) {
    const int dot = safe.lastIndexOf('.');
    if (dot > 0) safe.remove(dot);
    safe += FileFormats::extension(format);
  }
  return safe;
}

bool BrowserDownloadStore::begin(FileFormats::Format format, const String &suggestedName,
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

  if (byteLimit == 0 || byteLimit > BrowserLimits::MaxDownloadBytes) {
    byteLimit = BrowserLimits::MaxDownloadBytes;
  }
  if (declaredSize > 0 && static_cast<uint32_t>(declaredSize) > byteLimit) {
    error = "File exceeds download limit";
    return false;
  }
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) {
    error = "SD card is busy";
    return false;
  }

  const bool directoryReady = SD.exists(DownloadDirectory) || SD.mkdir(DownloadDirectory);
  if (!directoryReady) {
    SpiBusLock::unlock();
    error = "Cannot create Downloads directory";
    return false;
  }

  const String extension = FileFormats::extension(format);
  const uint32_t stamp = millis();
  for (uint16_t attempt = 0; attempt < MaxNameAttempts; ++attempt) {
    String name = suggestedName;
    if (!name.length()) name = String(F("media-")) + String(static_cast<unsigned long>(stamp)) + extension;
    if (attempt) {
      const int dot = name.lastIndexOf('.');
      const String suffix = dot > 0 ? name.substring(dot) : extension;
      const String stem = dot > 0 ? name.substring(0, dot) : name;
      name = stem + "-" + String(attempt) + suffix;
    }
    const String finalPath = String(DownloadDirectory) + "/" + name;
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
  _active = true;
  return true;
}

bool BrowserDownloadStore::write(const uint8_t *bytes, size_t length, String &error) {
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
    const size_t written = _file.write(bytes + offset, amount);
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

bool BrowserDownloadStore::commit(String &finalPath, String &error) {
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

void BrowserDownloadStore::abort() {
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

bool BrowserDownloadStore::discard(const String &path) {
  if (!_sdCard.isMounted() || !path.startsWith(String(DownloadDirectory) + "/") ||
      path.endsWith(F(".tmp")) ||
      !SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
  const bool removed = !SD.exists(path.c_str()) || SD.remove(path.c_str());
  SpiBusLock::unlock();
  return removed;
}

bool BrowserDownloadStore::validateTemporaryFile() {
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

void BrowserDownloadStore::reset() {
  _file = fs::File();
  _format = FileFormats::Format::Unknown;
  _finalPath = "";
  _temporaryPath = "";
  _byteLimit = 0;
  _bytesWritten = 0;
  _active = false;
}
