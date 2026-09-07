#pragma once

#include <Arduino.h>
#include <FS.h>

#include "CachePath.h"
#include "SourceFingerprint.h"
#include "../services/storage/SecretStorage.h"

class MediaCache {
public:
  enum class Result : uint8_t { Complete, More, Busy, Missing, Failed };

  struct Writer {
    StorageFile file;
    CachePaths paths;
    bool encrypted = false;
    bool active = false;
  };

  struct Cleanup {
    String sourcePath;
    uint8_t nextPath = 0;
    bool active = false;
  };

  explicit MediaCache(SecretStorage &storage) : _storage(storage) {}

  Result captureFingerprint(StorageFile &source, SourceFingerprint &fingerprint) const;
  static bool matches(const SourceFingerprint &stored, const SourceFingerprint &source) {
    return stored.matches(source);
  }

  Result openRead(const CachePaths &paths, bool encrypted, StorageFile &file) const;
  Result fileSize(StorageFile &file, uint32_t &size) const;
  Result read(StorageFile &file, void *buffer, size_t length) const;
  Result write(StorageFile &file, const void *buffer, size_t length) const;
  Result seek(StorageFile &file, uint32_t position) const;
  Result close(StorageFile &file) const;

  Result beginWrite(const CachePaths &paths, bool encrypted, Writer &writer) const;
  Result write(Writer &writer, const void *buffer, size_t length) const;
  Result seek(Writer &writer, uint32_t position) const;
  Result commit(Writer &writer) const;
  Result abort(Writer &writer) const;

  void beginCleanup(const String &sourcePath, Cleanup &cleanup) const;
  Result cleanupStep(Cleanup &cleanup) const;

private:
  static constexpr uint32_t LockTimeoutMs = 20;
  SecretStorage &_storage;
};
