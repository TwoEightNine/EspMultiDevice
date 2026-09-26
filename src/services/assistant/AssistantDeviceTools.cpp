#include "AssistantDeviceTools.h"

#include <FS.h>
#include <SD.h>
#include <ctype.h>

#include "../../core/SpiBusLock.h"
#include "../../media/FileFormats.h"
#include "../storage/SdCardController.h"
#include "../storage/SecretStorage.h"

namespace {
constexpr uint8_t MaxDirectoryDepth = 8;
constexpr uint32_t LockTimeoutMs = 100;

bool lockSd() {
  return SpiBusLock::lock(pdMS_TO_TICKS(LockTimeoutMs));
}

}  // namespace

AssistantDeviceTools::AssistantDeviceTools(SdCardController &sdCard)
    : _sdCard(sdCard), _selections{}, _selectionExpiresAt(0), _scanDeadlineMs(0), _scanBusy(false) {
  clearSelections();
}

void AssistantDeviceTools::clearSelections() {
  for (Selection &selection : _selections) {
    selection.path = "";
    selection.score = 0;
    selection.used = false;
  }
  _selectionExpiresAt = 0;
}

XiaozhiProtocol::DeviceToolResult AssistantDeviceTools::searchAudio(const char *queryText) {
  XiaozhiProtocol::DeviceToolResult result = {};
  result.clear();
  if (!queryText || !queryText[0] || strnlen(queryText, MaxQueryBytes + 1) > MaxQueryBytes) {
    result.error = XiaozhiProtocol::DeviceToolError::InvalidArguments;
    return result;
  }
  if (!_sdCard.isMounted()) {
    result.error = XiaozhiProtocol::DeviceToolError::StorageUnavailable;
    return result;
  }
  if (_scanBusy) {
    result.error = XiaozhiProtocol::DeviceToolError::Busy;
    return result;
  }

  // The wildcard is a bounded library pick for requests such as "play any
  // song". Ranking by path makes the choice stable across repeated searches.
  const String query = !strcmp(queryText, "*") ? String("*") : normalizedName(String(queryText));
  if (!query.length()) {
    result.error = XiaozhiProtocol::DeviceToolError::InvalidArguments;
    return result;
  }
  _scanBusy = true;
  _scanDeadlineMs = millis() + SearchDeadlineMs;
  clearSelections();
  const bool complete = scanDirectory("/", 0, query, result);
  _scanBusy = false;
  _scanDeadlineMs = 0;
  if (!complete) {
    clearSelections();
    if (result.error == XiaozhiProtocol::DeviceToolError::None)
      result.error = XiaozhiProtocol::DeviceToolError::StorageUnavailable;
    return result;
  }
  if (result.matchCount) {
    result.ok = true;
    _selectionExpiresAt = millis() + SelectionLifetimeMs;
  } else {
    result.ok = true;
  }
  return result;
}

XiaozhiProtocol::DeviceToolResult AssistantDeviceTools::playAudio(uint8_t selectionId,
                                                                    String &path) {
  XiaozhiProtocol::DeviceToolResult result = {};
  result.clear();
  path = "";
  if (!_sdCard.isMounted()) {
    result.error = XiaozhiProtocol::DeviceToolError::StorageUnavailable;
    return result;
  }
  if (!selectionValid(selectionId)) {
    result.error = XiaozhiProtocol::DeviceToolError::SelectionUnavailable;
    return result;
  }
  const String &candidate = _selections[selectionId - 1].path;
  if (hiddenOrPrivate(candidate) ||
      !FileFormats::isAudio(FileFormats::fromPath(candidate)) || !lockSd()) {
    result.error = XiaozhiProtocol::DeviceToolError::SelectionUnavailable;
    return result;
  }
  fs::File source = SD.open(candidate, FILE_READ);
  const bool valid = source && !source.isDirectory() && source.size() > 0;
  if (source) source.close();
  SpiBusLock::unlock();
  if (!valid) {
    result.error = XiaozhiProtocol::DeviceToolError::SelectionUnavailable;
    return result;
  }
  path = candidate;
  result.ok = true;
  result.accepted = true;
  clearSelections();
  return result;
}

bool AssistantDeviceTools::scanDirectory(const String &path, uint8_t depth, const String &query,
                                          XiaozhiProtocol::DeviceToolResult &result) {
  if (!_sdCard.isMounted()) return false;
  if (depth > MaxDirectoryDepth) return true;
  if (_scanDeadlineMs && static_cast<int32_t>(millis() - _scanDeadlineMs) >= 0) {
    result.error = XiaozhiProtocol::DeviceToolError::Busy;
    return false;
  }
  if (!lockSd()) {
    result.error = XiaozhiProtocol::DeviceToolError::Busy;
    return false;
  }
  fs::File directory = SD.open(path, FILE_READ);
  const bool usable = directory && directory.isDirectory();
  SpiBusLock::unlock();
  if (!usable) return false;

  for (;;) {
    if (_scanDeadlineMs && static_cast<int32_t>(millis() - _scanDeadlineMs) >= 0) {
      result.error = XiaozhiProtocol::DeviceToolError::Busy;
      if (lockSd()) { directory.close(); SpiBusLock::unlock(); }
      return false;
    }
    if (!_sdCard.isMounted()) {
      if (lockSd()) { directory.close(); SpiBusLock::unlock(); }
      return false;
    }
    if (!lockSd()) {
      result.error = XiaozhiProtocol::DeviceToolError::Busy;
      if (lockSd()) { directory.close(); SpiBusLock::unlock(); }
      return false;
    }
    fs::File entry = directory.openNextFile();
    if (!entry) { SpiBusLock::unlock(); break; }
    // On Arduino-ESP32, name() is only the basename; path() is the absolute
    // VFS path needed for recursion, private-folder checks, and playback.
    const String entryPath = entry.path() ? String(entry.path()) : String();
    const bool directoryEntry = entry.isDirectory();
    entry.close();
    SpiBusLock::unlock();
    if (entryPath.length() <= MaxPathBytes && !hiddenOrPrivate(entryPath)) {
      if (directoryEntry) {
        if (!scanDirectory(entryPath, depth + 1, query, result)) {
          if (lockSd()) { directory.close(); SpiBusLock::unlock(); }
          return false;
        }
      } else {
        considerPath(entryPath, query, result);
      }
    }
    vTaskDelay(1);
  }
  if (lockSd()) { directory.close(); SpiBusLock::unlock(); }
  return true;
}

void AssistantDeviceTools::considerPath(const String &path, const String &query,
                                         XiaozhiProtocol::DeviceToolResult &result) {
  const FileFormats::Format format = FileFormats::fromPath(path);
  if (!FileFormats::isAudio(format) || !FileFormats::isOpenable(format)) return;
  const uint16_t score = scoreMatch(normalizedName(baseName(path)), query);
  if (score) insertMatch(path, score, result);
}

void AssistantDeviceTools::insertMatch(const String &path, uint16_t score,
                                        XiaozhiProtocol::DeviceToolResult &result) {
  uint8_t slot = MaxMatches;
  for (uint8_t index = 0; index < MaxMatches; ++index) {
    if (!_selections[index].used) { slot = index; break; }
  }
  if (slot == MaxMatches) {
    Selection candidate = {path, score, true};
    if (!before(candidate, _selections[MaxMatches - 1])) return;
    slot = MaxMatches - 1;
  }
  _selections[slot] = {path, score, true};
  for (int8_t index = slot; index > 0 && before(_selections[index], _selections[index - 1]); --index) {
    Selection swap = _selections[index - 1];
    _selections[index - 1] = _selections[index];
    _selections[index] = swap;
  }
  result.matchCount = 0;
  for (uint8_t index = 0; index < MaxMatches && _selections[index].used; ++index) {
    XiaozhiProtocol::DeviceToolMatch &match = result.matches[result.matchCount++];
    match.selectionId = index + 1;
    copyDisplayName(_selections[index].path, match.name, sizeof(match.name));
  }
}

bool AssistantDeviceTools::before(const Selection &left, const Selection &right) {
  if (!left.used) return false;
  if (!right.used) return true;
  if (left.score != right.score) return left.score > right.score;
  return left.path.compareTo(right.path) < 0;
}

bool AssistantDeviceTools::selectionValid(uint8_t selectionId) const {
  return selectionId >= 1 && selectionId <= MaxMatches &&
         _selectionExpiresAt != 0 && static_cast<int32_t>(millis() - _selectionExpiresAt) < 0 &&
         _selections[selectionId - 1].used;
}

bool AssistantDeviceTools::hiddenOrPrivate(const String &path) {
  if (!path.startsWith("/") || path == "/" ||
      path == SecretStorage::Directory || path.startsWith(String(SecretStorage::Directory) + "/"))
    return true;
  int16_t start = 1;
  while (start < path.length()) {
    const int16_t slash = path.indexOf('/', start);
    const String segment = path.substring(start, slash < 0 ? path.length() : slash);
    if (segment.startsWith(".") || segment == "") return true;
    if (slash < 0) break;
    start = slash + 1;
  }
  return false;
}

String AssistantDeviceTools::baseName(const String &path) {
  const int16_t slash = path.lastIndexOf('/');
  String name = slash < 0 ? path : path.substring(slash + 1);
  const int16_t dot = name.lastIndexOf('.');
  if (dot > 0) name.remove(dot);
  return name;
}

String AssistantDeviceTools::normalizedName(const String &value) {
  String output;
  output.reserve(value.length());
  bool spacing = true;
  for (size_t index = 0; index < value.length(); ++index) {
    const char character = value[index];
    if ((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
        (character >= '0' && character <= '9') || static_cast<unsigned char>(character) >= 0x80) {
      output += static_cast<char>(tolower(static_cast<unsigned char>(character)));
      spacing = false;
    } else if (!spacing) {
      output += ' ';
      spacing = true;
    }
  }
  output.trim();
  return output;
}

uint16_t AssistantDeviceTools::scoreMatch(const String &name, const String &query) {
  if (!name.length() || !query.length()) return 0;
  if (query == "*") return 1;
  if (name == query) return 1000;
  if (name.startsWith(query)) return 850;
  if (name.indexOf(query) >= 0) return 700;

  uint8_t matched = 0;
  uint8_t required = 0;
  int16_t start = 0;
  while (start < query.length()) {
    const int16_t end = query.indexOf(' ', start);
    const String token = query.substring(start, end < 0 ? query.length() : end);
    if (token.length()) {
      ++required;
      if (name.indexOf(token) >= 0) ++matched;
    }
    if (end < 0) break;
    start = end + 1;
  }
  return required && matched == required ? static_cast<uint16_t>(400 + matched * 20) : 0;
}

void AssistantDeviceTools::copyDisplayName(const String &path, char *output, size_t outputBytes) {
  if (!output || outputBytes == 0) return;
  XiaozhiProtocol::copyUtf8Bounded(baseName(path).c_str(), output, outputBytes);
}
