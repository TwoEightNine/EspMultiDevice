#pragma once

#include <Arduino.h>

#include "FileFormats.h"

struct CachePaths {
  String directory;
  String finalPath;
  String temporaryPath;
};

namespace CachePath {
inline bool forSource(const String &sourcePath, FileFormats::Format cacheFormat,
                      CachePaths &paths) {
  paths = {"", "", ""};
  if (!FileFormats::isInternal(cacheFormat) || sourcePath.length() == 0) return false;
  const char *extension = FileFormats::extension(cacheFormat);
  if (!extension || extension[0] == '\0') return false;

  const int slash = sourcePath.lastIndexOf('/');
  const String name = slash < 0 ? sourcePath : sourcePath.substring(slash + 1);
  if (name.length() == 0) return false;
  const String parent = slash <= 0 ? String(F("/")) : sourcePath.substring(0, slash);
  paths.directory = parent == F("/") ? String(F("/.cache")) : parent + F("/.cache");
  paths.finalPath = paths.directory + F("/") + name + extension;
  paths.temporaryPath = paths.finalPath + F(".tmp");
  return true;
}
}  // namespace CachePath
