#pragma once

#include <Arduino.h>
#include <FS.h>

#include "../../media/FileFormats.h"

class SdCardController;

// One download transaction into a fixed SD folder: a `.tmp` file written in
// bounded `SpiBusLock` chunks, a byte cap, signature verification through
// `FileFormats`, and an atomic rename on commit. Anything unfinished is
// removed. The Browser saves to `/Downloads`; each SDK 6 app saves to its own
// `/Downloads/<app>` folder. Callers own naming and transport.
class DownloadStore {
public:
  static constexpr uint16_t WriteChunkBytes = 4096;

  // `directory` is an absolute SD path; missing levels are created on begin.
  DownloadStore(SdCardController &sdCard, const String &directory, uint32_t maxBytes);
  ~DownloadStore();
  DownloadStore(const DownloadStore &) = delete;
  DownloadStore &operator=(const DownloadStore &) = delete;

  // `name` is a plain file name, or empty for a generated one. An existing
  // name gets a numbered suffix. `byteLimit` 0 means the store's maximum.
  bool begin(FileFormats::Format format, const String &name, int32_t declaredSize,
             uint32_t byteLimit, String &error);
  bool write(const uint8_t *bytes, size_t length, String &error);
  bool commit(String &finalPath, String &error);
  void abort();
  // Removes a committed file in this store's folder.
  bool discard(const String &path);
  uint32_t bytesWritten() const { return _bytesWritten; }
  uint32_t writeElapsedMs() const { return _writeElapsedMs; }
  const String &directory() const { return _directory; }

private:
  static constexpr uint16_t MaxNameAttempts = 1000;

  SdCardController &_sdCard;
  String _directory;
  uint32_t _maxBytes;
  fs::File _file;
  FileFormats::Format _format;
  String _finalPath;
  String _temporaryPath;
  uint32_t _byteLimit;
  uint32_t _bytesWritten;
  uint32_t _writeElapsedMs;
  bool _active;

  bool ensureDirectory();  // Caller holds SpiBusLock.
  bool validateTemporaryFile();
  void reset();
};
