#include "AudioQueue.h"

#include <SD.h>
#include <esp_heap_caps.h>
#include <string.h>

#include <algorithm>

#include "../../core/NaturalOrder.h"
#include "../../core/SpiBusLock.h"
#include "../../media/FileFormats.h"

namespace {
String lastSegment(const String &path) {
  const int slash = path.lastIndexOf('/');
  return slash < 0 ? path : path.substring(slash + 1);
}
}  // namespace

AudioQueue::AudioQueue(SecretStorage &secretStorage)
  : _secretStorage(secretStorage),
    _phase(Phase::Idle),
    _secretAccess(false),
    _entries(nullptr),
    _pool(nullptr),
    _poolUsed(0),
    _count(0),
    _current(-1),
    _truncated(false) {
}

AudioQueue::~AudioQueue() {
  reset();
}

String AudioQueue::plainName(SecretStorage &secretStorage, const String &path, bool secretAccess) {
  const String stored = lastSegment(path);
  if (!secretAccess || !secretStorage.isSecretPath(path)) return stored;
  String decrypted;
  if (!secretStorage.decryptName(stored, decrypted)) return String();
  return secretStorage.displayName(decrypted);
}

void AudioQueue::reset() {
  closeDirectory();
  heap_caps_free(_entries);
  heap_caps_free(_pool);
  _entries = nullptr;
  _pool = nullptr;
  _poolUsed = 0;
  _count = 0;
  _current = -1;
  _truncated = false;
  _phase = Phase::Idle;
}

void AudioQueue::begin(const String &path, bool secretAccess) {
  reset();
  _secretAccess = secretAccess && _secretStorage.isSecretPath(path);
  const int slash = path.lastIndexOf('/');
  _directory = slash <= 0 ? String("/") : path.substring(0, slash);
  _currentName = lastSegment(path);
  // Names in a hidden folder can only be read while it is unlocked.
  if (_currentName.length() == 0 || (_secretAccess && !_secretStorage.unlocked())) {
    _phase = Phase::Failed;
    return;
  }
  _entries = static_cast<Entry *>(
    heap_caps_malloc(sizeof(Entry) * MaxTracks, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  _pool = static_cast<char *>(heap_caps_malloc(PoolBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!_entries || !_pool) {
    Serial.println(F("[Audio queue] no PSRAM"));
    reset();
    _phase = Phase::Failed;
    return;
  }
  _phase = Phase::Opening;
}

bool AudioQueue::step() {
  if (_phase == Phase::Opening) {
    if (!SpiBusLock::lock(0)) return false;
    _dir = SD.open(_directory.c_str());
    const bool valid = _dir && _dir.isDirectory();
    if (!valid && _dir) _dir.close();
    SpiBusLock::unlock();
    if (!valid) {
      _dir = fs::File();
      Serial.printf("[Audio queue] cannot list %s\n", _directory.c_str());
      _phase = Phase::Failed;
      return false;
    }
    _phase = Phase::Reading;
    return false;
  }
  if (_phase != Phase::Reading) return false;

  for (uint8_t i = 0; i < EntriesPerStep; i++) {
    if (!SpiBusLock::lock(0)) return false;
    fs::File entry = _dir.openNextFile();
    String name;
    bool directory = false;
    const bool more = (bool)entry;
    if (more) {
      name = lastSegment(String(entry.name()));
      directory = entry.isDirectory();
      entry.close();
    } else {
      _dir.close();
      _dir = fs::File();
    }
    SpiBusLock::unlock();
    if (!more) {
      finish();
      return true;
    }
    if (!directory) add(name);
  }
  return false;
}

void AudioQueue::add(const String &storageName) {
  // Hidden entries, macOS "._" files, the .cache folder, and .tmp writes.
  if (storageName.length() == 0 || storageName[0] == '.' || storageName.endsWith(F(".tmp"))) return;
  String decrypted = storageName;
  if (_secretAccess && !_secretStorage.decryptName(storageName, decrypted)) return;
  if (decrypted.length() == 0 || decrypted[0] == '.') return;
  const FileFormats::Format format = FileFormats::fromPath(decrypted);
  if (!FileFormats::isAudio(format) || !FileFormats::isOpenable(format)) return;
  const String plain = _secretAccess ? _secretStorage.displayName(decrypted) : decrypted;

  const uint32_t needed = storageName.length() + 1 + (_secretAccess ? plain.length() + 1 : 0);
  if (_count >= MaxTracks || PoolBytes - _poolUsed < needed) {
    _truncated = true;
    return;
  }
  Entry &entry = _entries[_count++];
  entry.storage = store(storageName);
  // A normal folder's display name is its storage name.
  entry.plain = _secretAccess ? store(plain) : entry.storage;
}

uint32_t AudioQueue::store(const String &text) {
  const uint32_t offset = _poolUsed;
  memcpy(_pool + offset, text.c_str(), text.length() + 1);
  _poolUsed += text.length() + 1;
  return offset;
}

void AudioQueue::finish() {
  const char *pool = _pool;
  std::sort(_entries, _entries + _count, [pool](const Entry &left, const Entry &right) {
    const char *a = pool + left.plain;
    const char *b = pool + right.plain;
    return NaturalOrder::compare(a, strlen(a), b, strlen(b)) < 0;
  });
  _current = -1;
  for (uint16_t i = 0; i < _count; i++) {
    if (_currentName == (pool + _entries[i].storage)) {
      _current = (int16_t)i;
      break;
    }
  }
  _phase = Phase::Done;
  Serial.printf("[Audio queue] %u tracks%s, current %d: %s\n", _count,
                _truncated ? " (truncated)" : "", _current, _directory.c_str());
}

void AudioQueue::setCurrent(uint16_t index) {
  if (index < _count) _current = (int16_t)index;
}

String AudioQueue::storagePath(uint16_t index) const {
  if (index >= _count) return String();
  const String name(_pool + _entries[index].storage);
  return _directory == "/" ? "/" + name : _directory + "/" + name;
}

String AudioQueue::displayName(uint16_t index) const {
  return index < _count ? String(_pool + _entries[index].plain) : String();
}

void AudioQueue::closeDirectory() {
  if (!_dir) return;
  SpiBusLock::lock(portMAX_DELAY);
  _dir.close();
  SpiBusLock::unlock();
  _dir = fs::File();
}
