#pragma once

#include <Arduino.h>
#include <FS.h>

#include "../../services/storage/SecretStorage.h"

// The Audio Player's play queue: the playable audio files in the current
// track's folder, in Files' natural name order. It is built by the player
// itself, so callers keep pushing a single audio path.
//
// The scan reads a few directory entries per step(), each under SpiBusLock,
// and keeps at most MaxTracks names in PSRAM. In a hidden-files folder the
// stored names are encrypted; the queue decrypts them for sorting, format
// detection, and display, which needs SecretStorage to be unlocked.
class AudioQueue {
public:
  static const uint16_t MaxTracks = 512;
  static const uint32_t PoolBytes = 64UL * 1024UL;

  explicit AudioQueue(SecretStorage &secretStorage);
  ~AudioQueue();

  // Starts scanning the folder of `path` (a storage path, as Files pushes it).
  void begin(const String &path, bool secretAccess);
  // Closes the folder and frees the names.
  void reset();
  // Reads up to EntriesPerStep entries. Returns true on the step that
  // completes the scan.
  bool step();

  bool scanning() const { return _phase == Phase::Opening || _phase == Phase::Reading; }
  // The scan finished and found the current track.
  bool ready() const { return _phase == Phase::Done && _current >= 0; }
  uint16_t count() const { return _count; }
  int16_t current() const { return _current; }
  void setCurrent(uint16_t index);
  String storagePath(uint16_t index) const;
  String displayName(uint16_t index) const;

  // The plain, displayable name of a storage path: decrypted in a hidden
  // folder, otherwise its last segment. Empty when it cannot be decrypted.
  static String plainName(SecretStorage &secretStorage, const String &path, bool secretAccess);

private:
  static const uint8_t EntriesPerStep = 4;

  enum class Phase : uint8_t {
    Idle,
    Opening,
    Reading,
    Done,
    Failed
  };

  struct Entry {
    uint32_t storage;  // Offsets of NUL-terminated names in _pool.
    uint32_t plain;
  };

  SecretStorage &_secretStorage;
  Phase _phase;
  bool _secretAccess;
  String _directory;
  String _currentName;
  fs::File _dir;
  Entry *_entries;
  char *_pool;
  uint32_t _poolUsed;
  uint16_t _count;
  int16_t _current;
  bool _truncated;

  void add(const String &storageName);
  uint32_t store(const String &text);
  void finish();
  void closeDirectory();
};
