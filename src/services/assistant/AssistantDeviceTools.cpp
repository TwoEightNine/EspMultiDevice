#include "AssistantDeviceTools.h"

#include <FS.h>
#include <SD.h>
#include <ctype.h>
#include <string.h>

#include "../../core/SpiBusLock.h"
#include "../../media/FileFormats.h"
#include "../storage/SdCardController.h"
#include "../storage/PathPolicy.h"
#include "../storage/SecretStorage.h"

namespace {
constexpr uint8_t MaxDirectoryDepth = 8;
constexpr uint32_t LockTimeoutMs = 100;

bool lockSd() {
  return SpiBusLock::lock(pdMS_TO_TICKS(LockTimeoutMs));
}

void copyFileType(const char *name, FileFormats::Format format, char *output, size_t capacity) {
  const char *known = FileFormats::extension(format);
  if (known && known[0]) {
    strncpy(output, known, capacity - 1);
    return;
  }
  const char *dot = name ? strrchr(name, '.') : nullptr;
  if (dot && dot[1] && strlen(dot) < capacity) {
    size_t index = 1;
    for (; dot[index]; ++index) {
      if (!isalnum(static_cast<unsigned char>(dot[index]))) break;
    }
    if (!dot[index]) {
      for (index = 0; dot[index]; ++index)
        output[index] = static_cast<char>(tolower(static_cast<unsigned char>(dot[index])));
      output[index] = 0;
      return;
    }
  }
  strncpy(output, "unknown", capacity - 1);
}

}  // namespace

AssistantDeviceTools::AssistantDeviceTools(SdCardController &sdCard)
    : _sdCard(sdCard), _selections{}, _selectionExpiresAt(0), _scanDeadlineMs(0),
      _scanBusy(false), _scanCancelled(nullptr), _scanCancelContext(nullptr),
      _searchKind(XiaozhiProtocol::DeviceCommandKind::None), _listingDirectory(),
      _listingPath(), _listingPendingPath(), _listingExpectedCursor(0),
      _listingExpiresAt(0), _listingSelectionId(0), _listingPendingDirectory(false),
      _listingHasPending(false) {
  clearSelections();
}

AssistantDeviceTools::~AssistantDeviceTools() {
  closeDirectoryListing();
}

void AssistantDeviceTools::clearSelections() {
  for (Selection &selection : _selections) {
    selection.path = "";
    selection.score = 0;
    selection.used = false;
    selection.directory = false;
  }
  _selectionExpiresAt = 0;
  _searchKind = XiaozhiProtocol::DeviceCommandKind::None;
}

bool AssistantDeviceTools::closeDirectoryListing() {
  bool closed = true;
  if (_listingDirectory) {
    if (lockSd()) {
      _listingDirectory.close();
      SpiBusLock::unlock();
    } else {
      closed = false;
    }
  }
  if (closed) _listingPath = "";
  _listingPendingPath = "";
  _listingExpectedCursor = 0;
  _listingExpiresAt = 0;
  _listingSelectionId = 0;
  _listingPendingDirectory = false;
  _listingHasPending = false;
  return closed;
}

void AssistantDeviceTools::clearDirectoryListing() {
  closeDirectoryListing();
}

XiaozhiProtocol::DeviceToolResult AssistantDeviceTools::searchAudio(const char *queryText) {
  return search(queryText, XiaozhiProtocol::DeviceCommandKind::SearchAudio);
}

XiaozhiProtocol::DeviceToolResult AssistantDeviceTools::search(
    const char *queryText, XiaozhiProtocol::DeviceCommandKind kind,
    bool (*cancelled)(void *), void *cancelContext) {
  XiaozhiProtocol::DeviceToolResult result = {};
  result.clear();
  result.command = kind;
  // Every new search invalidates the previous opaque selection set, even if
  // validation or storage access fails before scanning can begin.
  clearSelections();
  if (kind != XiaozhiProtocol::DeviceCommandKind::SearchAudio &&
      kind != XiaozhiProtocol::DeviceCommandKind::SearchImages &&
      kind != XiaozhiProtocol::DeviceCommandKind::SearchVideos &&
      kind != XiaozhiProtocol::DeviceCommandKind::SearchFiles) {
    result.error = XiaozhiProtocol::DeviceToolError::InvalidArguments;
    return result;
  }
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
  _scanCancelled = cancelled;
  _scanCancelContext = cancelContext;
  _scanDeadlineMs = millis() + SearchDeadlineMs;
  _searchKind = kind;
  const bool complete = scanDirectory("/", 0, query, result);
  _scanBusy = false;
  _scanCancelled = nullptr;
  _scanCancelContext = nullptr;
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
  return selected(selectionId, XiaozhiProtocol::DeviceCommandKind::PlayAudio, path);
}

XiaozhiProtocol::DeviceToolResult AssistantDeviceTools::selected(
    uint8_t selectionId, XiaozhiProtocol::DeviceCommandKind kind, String &path) {
  XiaozhiProtocol::DeviceToolResult result = {};
  result.clear();
  result.command = kind;
  path = "";
  if (!_sdCard.isMounted()) {
    result.error = XiaozhiProtocol::DeviceToolError::StorageUnavailable;
    return result;
  }
  if (!selectionValid(selectionId)) {
    result.error = XiaozhiProtocol::DeviceToolError::SelectionUnavailable;
    return result;
  }
  const Selection &selection = _selections[selectionId - 1];
  const String &candidate = selection.path;
  const bool expected =
      (kind == XiaozhiProtocol::DeviceCommandKind::PlayAudio &&
       _searchKind == XiaozhiProtocol::DeviceCommandKind::SearchAudio && !selection.directory) ||
      (kind == XiaozhiProtocol::DeviceCommandKind::OpenImage &&
       _searchKind == XiaozhiProtocol::DeviceCommandKind::SearchImages && !selection.directory) ||
      (kind == XiaozhiProtocol::DeviceCommandKind::PlayVideo &&
       _searchKind == XiaozhiProtocol::DeviceCommandKind::SearchVideos && !selection.directory) ||
      (kind == XiaozhiProtocol::DeviceCommandKind::ShowFileInfo &&
       _searchKind == XiaozhiProtocol::DeviceCommandKind::SearchFiles && !selection.directory) ||
      (kind == XiaozhiProtocol::DeviceCommandKind::OpenFolder &&
       _searchKind == XiaozhiProtocol::DeviceCommandKind::SearchFiles && selection.directory);
  const FileFormats::Format format = FileFormats::fromPath(candidate);
  if (!expected || hiddenOrPrivate(candidate) ||
      (!selection.directory && FileFormats::isInternal(format)) ||
      (kind == XiaozhiProtocol::DeviceCommandKind::PlayAudio && !FileFormats::isAudio(format)) ||
      (kind == XiaozhiProtocol::DeviceCommandKind::OpenImage && !FileFormats::isImage(format)) ||
      (kind == XiaozhiProtocol::DeviceCommandKind::PlayVideo && !FileFormats::isVideo(format)) ||
      !lockSd()) {
    result.error = XiaozhiProtocol::DeviceToolError::SelectionUnavailable;
    return result;
  }
  fs::File source = SD.open(candidate, FILE_READ);
  const bool valid = source && source.isDirectory() == selection.directory &&
      (selection.directory || source.size() > 0 || kind == XiaozhiProtocol::DeviceCommandKind::ShowFileInfo);
  if (valid && kind == XiaozhiProtocol::DeviceCommandKind::ShowFileInfo) {
    XiaozhiProtocol::copyUtf8Bounded(source.name(), result.name, sizeof(result.name));
    copyFileType(source.name(), format, result.fileType, sizeof(result.fileType));
    result.sizeBytes = source.size();
    result.modifiedUnix = source.getLastWrite();
    result.modifiedKnown = result.modifiedUnix > 0;
  }
  if (source) source.close();
  SpiBusLock::unlock();
  if (!valid) {
    result.error = XiaozhiProtocol::DeviceToolError::SelectionUnavailable;
    return result;
  }
  if (kind != XiaozhiProtocol::DeviceCommandKind::ShowFileInfo) path = candidate;
  result.ok = true;
  result.accepted = kind != XiaozhiProtocol::DeviceCommandKind::ShowFileInfo;
  if (result.accepted) clearSelections();
  return result;
}

bool AssistantDeviceTools::scanDirectory(const String &path, uint8_t depth, const String &query,
                                          XiaozhiProtocol::DeviceToolResult &result) {
  if (_scanCancelled && _scanCancelled(_scanCancelContext)) {
    result.error = XiaozhiProtocol::DeviceToolError::Busy;
    return false;
  }
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
    if (_scanCancelled && _scanCancelled(_scanCancelContext)) {
      result.error = XiaozhiProtocol::DeviceToolError::Busy;
      if (lockSd()) { directory.close(); SpiBusLock::unlock(); }
      return false;
    }
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
        considerPath(entryPath, true, query, result);
        if (!scanDirectory(entryPath, depth + 1, query, result)) {
          if (lockSd()) { directory.close(); SpiBusLock::unlock(); }
          return false;
        }
      } else {
        considerPath(entryPath, false, query, result);
      }
    }
    vTaskDelay(1);
  }
  if (lockSd()) { directory.close(); SpiBusLock::unlock(); }
  return true;
}

void AssistantDeviceTools::considerPath(const String &path, bool directory, const String &query,
                                         XiaozhiProtocol::DeviceToolResult &result) {
  const FileFormats::Format format = FileFormats::fromPath(path);
  if (!directory && FileFormats::isInternal(format)) return;
  if (_searchKind == XiaozhiProtocol::DeviceCommandKind::SearchAudio &&
      (directory || !FileFormats::isAudio(format) || !FileFormats::isOpenable(format))) return;
  if (_searchKind == XiaozhiProtocol::DeviceCommandKind::SearchImages &&
      (directory || !FileFormats::isImage(format) || !FileFormats::isOpenable(format))) return;
  if (_searchKind == XiaozhiProtocol::DeviceCommandKind::SearchVideos &&
      (directory || !FileFormats::isVideo(format) || !FileFormats::isOpenable(format))) return;
  if (_searchKind != XiaozhiProtocol::DeviceCommandKind::SearchFiles && directory) return;
  uint16_t score = scoreMatch(normalizedName(baseName(path)), query);
  if (_searchKind == XiaozhiProtocol::DeviceCommandKind::SearchFiles) {
    const uint16_t pathScore = scoreMatch(normalizedName(path), query);
    if (pathScore > score) score = pathScore;
  }
  if (score) insertMatch(path, directory, score, result);
}

void AssistantDeviceTools::insertMatch(const String &path, bool directory, uint16_t score,
                                        XiaozhiProtocol::DeviceToolResult &result) {
  uint8_t slot = MaxMatches;
  for (uint8_t index = 0; index < MaxMatches; ++index) {
    if (!_selections[index].used) { slot = index; break; }
  }
  if (slot == MaxMatches) {
    Selection candidate = {path, score, true, directory};
    if (!before(candidate, _selections[MaxMatches - 1])) return;
    slot = MaxMatches - 1;
  }
  _selections[slot] = {path, score, true, directory};
  for (int8_t index = slot; index > 0 && before(_selections[index], _selections[index - 1]); --index) {
    Selection swap = _selections[index - 1];
    _selections[index - 1] = _selections[index];
    _selections[index] = swap;
  }
  result.matchCount = 0;
  for (uint8_t index = 0; index < MaxMatches && _selections[index].used; ++index) {
    XiaozhiProtocol::DeviceToolMatch &match = result.matches[result.matchCount++];
    match.selectionId = index + 1;
    if (_searchKind == XiaozhiProtocol::DeviceCommandKind::SearchFiles) {
      XiaozhiProtocol::copyUtf8Bounded(_selections[index].path.c_str(),
                                       match.path, sizeof(match.path));
    } else {
      copyDisplayName(_selections[index].path, match.name, sizeof(match.name));
    }
    match.directory = _selections[index].directory;
  }
}

bool AssistantDeviceTools::startDirectoryListing(
    uint8_t selectionId, XiaozhiProtocol::DeviceToolResult &result) {
  if (!closeDirectoryListing()) {
    result.error = XiaozhiProtocol::DeviceToolError::Busy;
    return false;
  }
  String path;
  if (selectionId == 0) {
    path = "/";
  } else {
    if (!selectionValid(selectionId) ||
        _searchKind != XiaozhiProtocol::DeviceCommandKind::SearchFiles ||
        !_selections[selectionId - 1].directory) {
      result.error = XiaozhiProtocol::DeviceToolError::SelectionUnavailable;
      return false;
    }
    path = _selections[selectionId - 1].path;
    if (hiddenOrPrivate(path)) {
      result.error = XiaozhiProtocol::DeviceToolError::SelectionUnavailable;
      return false;
    }
  }
  if (path.length() > MaxPathBytes || !lockSd()) {
    result.error = path.length() > MaxPathBytes
        ? XiaozhiProtocol::DeviceToolError::SelectionUnavailable
        : XiaozhiProtocol::DeviceToolError::Busy;
    return false;
  }
  _listingDirectory = SD.open(path, FILE_READ);
  const bool valid = _listingDirectory && _listingDirectory.isDirectory();
  if (!valid && _listingDirectory) _listingDirectory.close();
  SpiBusLock::unlock();
  if (!valid) {
    result.error = XiaozhiProtocol::DeviceToolError::SelectionUnavailable;
    return false;
  }
  _listingPath = path;
  _listingSelectionId = selectionId;
  _listingExpectedCursor = 0;
  _listingExpiresAt = millis() + SelectionLifetimeMs;
  return true;
}

AssistantDeviceTools::ListingRead AssistantDeviceTools::readDirectoryEntry(
    String &path, bool &directory, bool (*cancelled)(void *), void *cancelContext) {
  path = "";
  directory = false;
  if (cancelled && cancelled(cancelContext)) return ListingRead::Cancelled;
  if (!_sdCard.isMounted()) return ListingRead::Unavailable;
  if (_listingHasPending) {
    path = _listingPendingPath;
    directory = _listingPendingDirectory;
    _listingPendingPath = "";
    _listingPendingDirectory = false;
    _listingHasPending = false;
    return ListingRead::Found;
  }
  for (;;) {
    if (cancelled && cancelled(cancelContext)) return ListingRead::Cancelled;
    if (!_sdCard.isMounted()) return ListingRead::Unavailable;
    if (!lockSd()) return ListingRead::Busy;
    fs::File entry = _listingDirectory.openNextFile();
    if (!entry) {
      _listingDirectory.close();
      SpiBusLock::unlock();
      return ListingRead::End;
    }
    const String entryPath = entry.path() ? String(entry.path()) : String();
    const bool directoryEntry = entry.isDirectory();
    entry.close();
    SpiBusLock::unlock();
    if (entryPath.length() <= MaxPathBytes && !hiddenOrPrivate(entryPath) &&
        (directoryEntry || !FileFormats::isInternal(FileFormats::fromPath(entryPath)))) {
      path = entryPath;
      directory = directoryEntry;
      return ListingRead::Found;
    }
    vTaskDelay(1);
  }
}

XiaozhiProtocol::DeviceToolResult AssistantDeviceTools::listDirectory(
    uint8_t selectionId, uint32_t cursor, bool (*cancelled)(void *), void *cancelContext) {
  XiaozhiProtocol::DeviceToolResult result = {};
  result.clear();
  result.command = XiaozhiProtocol::DeviceCommandKind::ListDirectory;
  if (!_sdCard.isMounted()) {
    closeDirectoryListing();
    result.error = XiaozhiProtocol::DeviceToolError::StorageUnavailable;
    return result;
  }
  if (cursor == 0) {
    if (!startDirectoryListing(selectionId, result)) return result;
  } else {
    const bool expired = _listingExpiresAt == 0 ||
        static_cast<int32_t>(millis() - _listingExpiresAt) >= 0;
    if (!_listingDirectory || expired || selectionId != _listingSelectionId ||
        cursor != _listingExpectedCursor) {
      if (expired) closeDirectoryListing();
      result.error = XiaozhiProtocol::DeviceToolError::SelectionUnavailable;
      return result;
    }
  }
  XiaozhiProtocol::copyUtf8Bounded(_listingPath.c_str(), result.directoryPath,
                                   sizeof(result.directoryPath));
  while (result.matchCount < MaxMatches) {
    String path;
    bool directory = false;
    const ListingRead read = readDirectoryEntry(path, directory, cancelled, cancelContext);
    if (read == ListingRead::Found) {
      auto &entry = result.matches[result.matchCount++];
      XiaozhiProtocol::copyUtf8Bounded(path.c_str(), entry.path, sizeof(entry.path));
      entry.directory = directory;
      vTaskDelay(1);
      continue;
    }
    if (read == ListingRead::End) {
      result.ok = true;
      closeDirectoryListing();
      return result;
    }
    if (read == ListingRead::Unavailable) {
      closeDirectoryListing();
      result.error = XiaozhiProtocol::DeviceToolError::StorageUnavailable;
      return result;
    }
    if (read == ListingRead::Cancelled) closeDirectoryListing();
    if (read == ListingRead::Busy && result.matchCount) {
      _listingExpectedCursor = cursor == 0 ? 1 : cursor + 1;
      if (_listingExpectedCursor == 0) {
        closeDirectoryListing();
        result.error = XiaozhiProtocol::DeviceToolError::SelectionUnavailable;
        return result;
      }
      _listingExpiresAt = millis() + SelectionLifetimeMs;
      result.ok = true;
      result.hasNextCursor = true;
      result.nextCursor = _listingExpectedCursor;
      return result;
    }
    result.error = XiaozhiProtocol::DeviceToolError::Busy;
    return result;
  }

  String pendingPath;
  bool pendingDirectory = false;
  const ListingRead lookahead = readDirectoryEntry(
      pendingPath, pendingDirectory, cancelled, cancelContext);
  if (lookahead == ListingRead::Found) {
    _listingPendingPath = pendingPath;
    _listingPendingDirectory = pendingDirectory;
    _listingHasPending = true;
    _listingExpectedCursor = cursor == 0 ? 1 : cursor + 1;
    if (_listingExpectedCursor == 0) {
      closeDirectoryListing();
      result.error = XiaozhiProtocol::DeviceToolError::SelectionUnavailable;
      return result;
    }
    _listingExpiresAt = millis() + SelectionLifetimeMs;
    result.ok = true;
    result.hasNextCursor = true;
    result.nextCursor = _listingExpectedCursor;
    return result;
  }
  if (lookahead == ListingRead::End) {
    result.ok = true;
    closeDirectoryListing();
    return result;
  }
  if (lookahead == ListingRead::Unavailable) {
    closeDirectoryListing();
    result.error = XiaozhiProtocol::DeviceToolError::StorageUnavailable;
    return result;
  }
  if (lookahead == ListingRead::Busy) {
    _listingExpectedCursor = cursor == 0 ? 1 : cursor + 1;
    if (_listingExpectedCursor == 0) {
      closeDirectoryListing();
      result.error = XiaozhiProtocol::DeviceToolError::SelectionUnavailable;
      return result;
    }
    _listingExpiresAt = millis() + SelectionLifetimeMs;
    result.ok = true;
    result.hasNextCursor = true;
    result.nextCursor = _listingExpectedCursor;
    return result;
  }
  if (lookahead == ListingRead::Cancelled) closeDirectoryListing();
  result.error = XiaozhiProtocol::DeviceToolError::Busy;
  return result;
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
  return PathPolicy::hiddenOrPrivate(path);
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
