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

  // Moves published sidecars between source paths without exposing their
  // encryption mode to callers. One relocation step transfers at most one
  // caller-supplied payload chunk, so Files can keep touch polling responsive.
  struct Relocation {
    enum class Phase : uint8_t {
      CaptureDestination,
      CloseDestination,
      StartCache,
      CopyCache,
      UpdateFingerprint,
      SealCache,
      CloseCache,
      DiscardCache
    };

    String sourcePath;
    String fingerprintPath;
    String destinationPath;
    bool sourceEncrypted = false;
    bool destinationEncrypted = false;
    SourceFingerprint destinationFingerprint{};
    StorageFile destination;
    StorageFile source;
    Writer writer;
    uint32_t sourceBytes = 0;
    uint32_t copiedBytes = 0;
    uint8_t nextFormat = 0;
    uint8_t nextPublish = 0;
    uint8_t preparedFormats = 0;
    uint8_t failedCaches = 0;
    Phase phase = Phase::CaptureDestination;
    bool destinationCaptured = false;
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
  // Flushes and closes a temporary writer without publishing it. Used by a
  // staged file move so cache finals appear only after the primary source.
  Result seal(Writer &writer) const;
  Result commit(Writer &writer) const;
  Result abort(Writer &writer) const;

  void beginCleanup(const String &sourcePath, Cleanup &cleanup) const;
  Result cleanupStep(Cleanup &cleanup) const;

  // Cache sidecars store their source fingerprint at a format-specific
  // header offset. The helper is public so the cache contract sketch can
  // verify every persisted format without requiring an SD card.
  static bool rewriteFingerprint(FileFormats::Format format, uint8_t *header,
                                 size_t length, const SourceFingerprint &fingerprint) {
    uint8_t offset = 0;
    if (!header || !fingerprintOffset(format, offset) || length < (size_t)offset + 8) return false;
    writeLe32(header + offset, fingerprint.size);
    writeLe32(header + offset + 4, fingerprint.modified);
    return true;
  }

  void beginRelocation(const String &sourcePath, const String &fingerprintPath,
                       const String &destinationPath,
                       bool sourceEncrypted, bool destinationEncrypted,
                       Relocation &relocation) const;
  Result relocationStep(Relocation &relocation, uint8_t *buffer, size_t bufferBytes) const;
  Result publishRelocationStep(Relocation &relocation) const;
  Result discardRelocationStep(Relocation &relocation) const;
  void abandonRelocation(Relocation &relocation) const;

private:
  static constexpr uint32_t LockTimeoutMs = 20;
  SecretStorage &_storage;

  static bool fingerprintOffset(FileFormats::Format format, uint8_t &offset) {
    switch (format) {
      case FileFormats::Format::Eth1:
      case FileFormats::Format::Empa:
        offset = 8;
        return true;
      case FileFormats::Format::Em16:
      case FileFormats::Format::Em16Zoom:
        offset = 12;
        return true;
      default:
        offset = 0;
        return false;
    }
  }
  static void writeLe32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
  }
  static void advanceRelocation(Relocation &relocation);
};
