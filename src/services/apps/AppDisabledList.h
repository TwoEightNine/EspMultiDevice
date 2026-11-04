#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Arduino-free list of app IDs the user disabled on this device, oldest
// first. `AppSettings` persists its encoding; `tests/app_disabled_list`
// checks it on the host.
//
// Encoding: the IDs joined by '\n', with no terminator. A blob that does not
// decode cleanly (bad ID, duplicate, too many) is dropped as a whole, which
// re-enables every app rather than guessing.
class AppDisabledList {
public:
  static constexpr uint8_t MaxIds = 64;
  static constexpr size_t MaxIdBytes = 32;
  static constexpr size_t MaxEncodedBytes = MaxIds * (MaxIdBytes + 1);

  uint8_t count() const { return _count; }
  const char *id(uint8_t index) const { return index < _count ? _ids[index] : ""; }

  // Same rule as `AppPackage::validId`: [a-z][a-z0-9_-]{0,31}.
  static bool validId(const char *value) {
    if (!value) return false;
    const size_t length = strnlen(value, MaxIdBytes + 1);
    if (!length || length > MaxIdBytes || value[0] < 'a' || value[0] > 'z') return false;
    for (size_t i = 1; i < length; ++i) {
      const char c = value[i];
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
        return false;
    }
    return true;
  }

  bool contains(const char *value) const { return find(value) >= 0; }

  // Returns true when the list changed. Disabling a new ID while the list is
  // full drops the oldest entry, which re-enables that app.
  bool setDisabled(const char *value, bool disabled) {
    if (!validId(value)) return false;
    const int index = find(value);
    if (disabled) {
      if (index >= 0) return false;
      if (_count == MaxIds) removeAt(0);
      copyId(_ids[_count++], value);
      return true;
    }
    if (index < 0) return false;
    removeAt(static_cast<uint8_t>(index));
    return true;
  }

  void clear() { _count = 0; }

  // Writes the encoding into `out` (at least MaxEncodedBytes) and returns its
  // length.
  size_t encode(char *out, size_t capacity) const {
    size_t used = 0;
    for (uint8_t i = 0; i < _count; ++i) {
      const size_t length = strlen(_ids[i]);
      if (used + length + (i ? 1 : 0) > capacity) break;
      if (i) out[used++] = '\n';
      memcpy(out + used, _ids[i], length);
      used += length;
    }
    return used;
  }

  // Replaces the list with a decoded blob; false (and an empty list) when
  // the blob is malformed.
  bool decode(const char *data, size_t length) {
    _count = 0;
    if (!data || !length) return true;
    if (length > MaxEncodedBytes) return false;
    size_t start = 0;
    while (start <= length) {
      size_t end = start;
      while (end < length && data[end] != '\n') ++end;
      const size_t idLength = end - start;
      char value[MaxIdBytes + 1] = {};
      if (!idLength || idLength > MaxIdBytes || _count == MaxIds) { _count = 0; return false; }
      memcpy(value, data + start, idLength);
      if (!validId(value) || contains(value)) { _count = 0; return false; }
      copyId(_ids[_count++], value);
      if (end == length) break;
      start = end + 1;
    }
    return true;
  }

private:
  char _ids[MaxIds][MaxIdBytes + 1] = {};
  uint8_t _count = 0;

  int find(const char *value) const {
    if (!value) return -1;
    for (uint8_t i = 0; i < _count; ++i)
      if (strcmp(_ids[i], value) == 0) return i;
    return -1;
  }

  void removeAt(uint8_t index) {
    for (uint8_t i = index; i + 1 < _count; ++i) memcpy(_ids[i], _ids[i + 1], sizeof(_ids[i]));
    --_count;
  }

  static void copyId(char *target, const char *value) {
    const size_t length = strnlen(value, MaxIdBytes);
    memcpy(target, value, length);
    target[length] = '\0';
  }
};
