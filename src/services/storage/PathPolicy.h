#pragma once

#include <Arduino.h>

#include "SecretStorage.h"

// Shared rules for SD paths handed to components that must stay out of secret,
// hidden, and temporary storage: Assistant device tools and SDK 4 Lua apps.
class PathPolicy {
public:
  static constexpr size_t MaxPathBytes = 255;

  // True for names no ordinary listing exposes: dot-names (`.cache`,
  // `.configs`, `.system`, macOS metadata) and temporaries.
  static bool hiddenName(const String &name) {
    return name.length() == 0 || name.startsWith(".") || name.endsWith(".tmp");
  }

  static bool hiddenOrPrivate(const String &path) {
    if (!path.startsWith("/") || path == "/" ||
        path == SecretStorage::Directory || path.startsWith(String(SecretStorage::Directory) + "/"))
      return true;
    int16_t start = 1;
    while (start < path.length()) {
      const int16_t slash = path.indexOf('/', start);
      const String segment = path.substring(start, slash < 0 ? path.length() : slash);
      if (hiddenName(segment) || segment == ".configs") return true;
      if (slash < 0) break;
      start = slash + 1;
    }
    return false;
  }

  // Validates a path supplied by an SD app. Beyond hiddenOrPrivate() it
  // rejects everything FAT would silently map onto another name (trailing
  // dots/spaces), reserved characters, and control bytes, so the checked text
  // is the entry the filesystem will address. Root is accepted only on request.
  static bool appPath(const char *value, size_t length, bool allowRoot, String &path) {
    if (!value || length == 0 || length > MaxPathBytes || value[0] != '/') return false;
    if (length == 1) { path = "/"; return allowRoot; }
    size_t segmentStart = 1;
    for (size_t i = 1; i <= length; ++i) {
      const unsigned char c = i < length ? static_cast<unsigned char>(value[i]) : '/';
      if (c != '/') {
        if (c < 0x20 || c == 0x7f || strchr("\\:*?\"<>|", c)) return false;
        continue;
      }
      const size_t segmentLength = i - segmentStart;
      if (segmentLength == 0) return false;
      const char last = value[i - 1];
      if (value[segmentStart] == ' ' || last == ' ' || last == '.') return false;
      segmentStart = i + 1;
    }
    path = String(value);
    return path.length() == length && !hiddenOrPrivate(path);
  }

  // FAT names are case-insensitive, so ancestry is compared the same way.
  static bool atOrBelow(const String &path, const String &ancestor) {
    if (path.length() < ancestor.length()) return false;
    if (!path.substring(0, ancestor.length()).equalsIgnoreCase(ancestor)) return false;
    return path.length() == ancestor.length() || path[ancestor.length()] == '/';
  }

  // SD apps may list and open package folders but never change them.
  static bool appMutable(const String &path) {
    return path != "/" && !atOrBelow(path, "/Apps");
  }
};
