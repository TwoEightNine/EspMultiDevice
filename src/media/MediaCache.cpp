#include "MediaCache.h"

#include "../core/SpiBusLock.h"

MediaCache::Result MediaCache::captureFingerprint(StorageFile &source,
                                                   SourceFingerprint &fingerprint) const {
  if (!source) return Result::Failed;
  if (!SpiBusLock::lock(pdMS_TO_TICKS(LockTimeoutMs))) return Result::Busy;
  fingerprint.size = (uint32_t)source.size();
  fingerprint.modified = source.getLastWrite();
  SpiBusLock::unlock();
  return Result::Complete;
}

MediaCache::Result MediaCache::openRead(const CachePaths &paths, bool encrypted,
                                        StorageFile &file) const {
  if (paths.finalPath.length() == 0) return Result::Failed;
  if (!SpiBusLock::lock(pdMS_TO_TICKS(LockTimeoutMs))) return Result::Busy;
  const bool exists = _storage.exists(paths.finalPath);
  if (exists) file = _storage.open(paths.finalPath, FILE_READ, encrypted);
  SpiBusLock::unlock();
  if (!exists) return Result::Missing;
  return file ? Result::Complete : Result::Failed;
}

MediaCache::Result MediaCache::fileSize(StorageFile &file, uint32_t &size) const {
  if (!file) return Result::Failed;
  if (!SpiBusLock::lock(pdMS_TO_TICKS(LockTimeoutMs))) return Result::Busy;
  size = (uint32_t)file.size();
  SpiBusLock::unlock();
  return Result::Complete;
}

MediaCache::Result MediaCache::read(StorageFile &file, void *buffer, size_t length) const {
  if (!file || !buffer) return Result::Failed;
  if (!SpiBusLock::lock(pdMS_TO_TICKS(LockTimeoutMs))) return Result::Busy;
  const bool ok = file.read(buffer, length) == (int)length;
  SpiBusLock::unlock();
  return ok ? Result::Complete : Result::Failed;
}

MediaCache::Result MediaCache::write(StorageFile &file, const void *buffer, size_t length) const {
  if (!file || !buffer) return Result::Failed;
  if (!SpiBusLock::lock(pdMS_TO_TICKS(LockTimeoutMs))) return Result::Busy;
  const bool ok = file.write(buffer, length) == length;
  SpiBusLock::unlock();
  return ok ? Result::Complete : Result::Failed;
}

MediaCache::Result MediaCache::seek(StorageFile &file, uint32_t position) const {
  if (!file) return Result::Failed;
  if (!SpiBusLock::lock(pdMS_TO_TICKS(LockTimeoutMs))) return Result::Busy;
  const bool ok = file.seek(position);
  SpiBusLock::unlock();
  return ok ? Result::Complete : Result::Failed;
}

MediaCache::Result MediaCache::close(StorageFile &file) const {
  if (!file) return Result::Complete;
  if (!SpiBusLock::lock(pdMS_TO_TICKS(LockTimeoutMs))) return Result::Busy;
  file.close();
  SpiBusLock::unlock();
  return Result::Complete;
}

MediaCache::Result MediaCache::beginWrite(const CachePaths &paths, bool encrypted,
                                          Writer &writer) const {
  if (writer.active || paths.directory.length() == 0 || paths.temporaryPath.length() == 0) {
    return Result::Failed;
  }
  if (!SpiBusLock::lock(pdMS_TO_TICKS(LockTimeoutMs))) return Result::Busy;
  bool ready = _storage.mkdir(paths.directory);
  if (ready && _storage.exists(paths.temporaryPath)) ready = _storage.remove(paths.temporaryPath);
  StorageFile file = ready ? _storage.open(paths.temporaryPath, FILE_WRITE, encrypted) : StorageFile();
  SpiBusLock::unlock();
  if (!file) return Result::Failed;
  writer.file = file;
  writer.paths = paths;
  writer.encrypted = encrypted;
  writer.active = true;
  return Result::Complete;
}

MediaCache::Result MediaCache::write(Writer &writer, const void *buffer, size_t length) const {
  return writer.active ? write(writer.file, buffer, length) : Result::Failed;
}

MediaCache::Result MediaCache::seek(Writer &writer, uint32_t position) const {
  return writer.active ? seek(writer.file, position) : Result::Failed;
}

MediaCache::Result MediaCache::seal(Writer &writer) const {
  if (!writer.active || !writer.file) return Result::Failed;
  if (!SpiBusLock::lock(pdMS_TO_TICKS(LockTimeoutMs))) return Result::Busy;
  writer.file.flush();
  writer.file.close();
  SpiBusLock::unlock();
  writer.file = StorageFile();
  return Result::Complete;
}

MediaCache::Result MediaCache::commit(Writer &writer) const {
  if (!writer.active || !writer.file) return Result::Failed;
  if (!SpiBusLock::lock(pdMS_TO_TICKS(LockTimeoutMs))) return Result::Busy;
  writer.file.flush();
  writer.file.close();
  bool ready = !_storage.exists(writer.paths.finalPath) || _storage.remove(writer.paths.finalPath);
  const bool committed = ready && _storage.rename(writer.paths.temporaryPath, writer.paths.finalPath);
  if (!committed) _storage.remove(writer.paths.temporaryPath);
  SpiBusLock::unlock();
  writer.active = false;
  writer.paths = {"", "", ""};
  return committed ? Result::Complete : Result::Failed;
}

MediaCache::Result MediaCache::abort(Writer &writer) const {
  if (!writer.active) return Result::Complete;
  if (!SpiBusLock::lock(pdMS_TO_TICKS(LockTimeoutMs))) return Result::Busy;
  if (writer.file) writer.file.close();
  const bool removed = !_storage.exists(writer.paths.temporaryPath) ||
                       _storage.remove(writer.paths.temporaryPath);
  SpiBusLock::unlock();
  writer.active = false;
  writer.paths = {"", "", ""};
  return removed ? Result::Complete : Result::Failed;
}

void MediaCache::beginCleanup(const String &sourcePath, Cleanup &cleanup) const {
  cleanup.sourcePath = sourcePath;
  cleanup.nextPath = 0;
  cleanup.active = sourcePath.length() > 0;
}

MediaCache::Result MediaCache::cleanupStep(Cleanup &cleanup) const {
  if (!cleanup.active) return cleanup.sourcePath.length() ? Result::Complete : Result::Failed;
  const uint8_t count = FileFormats::internalCacheCount() * 2;
  if (cleanup.nextPath >= count) {
    cleanup.active = false;
    return Result::Complete;
  }
  CachePaths paths;
  const FileFormats::Format format = FileFormats::internalCacheAt(cleanup.nextPath / 2);
  if (!CachePath::forSource(cleanup.sourcePath, format, paths)) return Result::Failed;
  const String &path = (cleanup.nextPath & 1) ? paths.temporaryPath : paths.finalPath;
  if (!SpiBusLock::lock(pdMS_TO_TICKS(LockTimeoutMs))) return Result::Busy;
  const bool removed = !_storage.exists(path) || _storage.remove(path);
  SpiBusLock::unlock();
  if (!removed) return Result::Failed;
  cleanup.nextPath++;
  if (cleanup.nextPath == count) {
    cleanup.active = false;
    return Result::Complete;
  }
  return Result::More;
}

void MediaCache::beginRelocation(const String &sourcePath, const String &fingerprintPath,
                                 const String &destinationPath,
                                 bool sourceEncrypted, bool destinationEncrypted,
                                 Relocation &relocation) const {
  relocation = Relocation();
  relocation.sourcePath = sourcePath;
  relocation.fingerprintPath = fingerprintPath;
  relocation.destinationPath = destinationPath;
  relocation.sourceEncrypted = sourceEncrypted;
  relocation.destinationEncrypted = destinationEncrypted;
  relocation.active = sourcePath.length() > 0 && fingerprintPath.length() > 0 &&
                      destinationPath.length() > 0;
}

void MediaCache::advanceRelocation(Relocation &relocation) {
  relocation.source = StorageFile();
  relocation.writer = Writer();
  relocation.sourceBytes = 0;
  relocation.copiedBytes = 0;
  relocation.nextFormat++;
  relocation.phase = Relocation::Phase::StartCache;
}

MediaCache::Result MediaCache::relocationStep(Relocation &relocation, uint8_t *buffer,
                                               size_t bufferBytes) const {
  if (!relocation.active || !buffer || bufferBytes == 0) return Result::Failed;

  if (relocation.phase == Relocation::Phase::CaptureDestination) {
    if (!relocation.destination) {
      if (!SpiBusLock::lock(pdMS_TO_TICKS(LockTimeoutMs))) return Result::Busy;
      relocation.destination = _storage.open(relocation.fingerprintPath, FILE_READ,
                                             relocation.destinationEncrypted);
      SpiBusLock::unlock();
      if (!relocation.destination) {
        // The primary staged file is no longer available. Caches are optional,
        // so allow the caller to publish the primary move without them.
        relocation.active = false;
        return Result::Complete;
      }
    }
    const Result captured = captureFingerprint(relocation.destination,
                                               relocation.destinationFingerprint);
    if (captured == Result::Busy) return Result::Busy;
    relocation.destinationCaptured = captured == Result::Complete;
    relocation.phase = Relocation::Phase::CloseDestination;
    return Result::More;
  }

  if (relocation.phase == Relocation::Phase::CloseDestination) {
    const Result closed = close(relocation.destination);
    if (closed == Result::Busy) return Result::Busy;
    if (!relocation.destinationCaptured) {
      relocation.active = false;
      return Result::Complete;
    }
    relocation.phase = Relocation::Phase::StartCache;
    return Result::More;
  }

  if (relocation.nextFormat >= FileFormats::internalCacheCount()) {
    relocation.active = false;
    return Result::Complete;
  }

  const FileFormats::Format format = FileFormats::internalCacheAt(relocation.nextFormat);
  if (relocation.phase == Relocation::Phase::StartCache) {
    CachePaths sourcePaths;
    CachePaths destinationPaths;
    uint8_t fingerprintAt = 0;
    if (!CachePath::forSource(relocation.sourcePath, format, sourcePaths) ||
        !CachePath::forSource(relocation.destinationPath, format, destinationPaths) ||
        !fingerprintOffset(format, fingerprintAt)) {
      relocation.failedCaches++;
      advanceRelocation(relocation);
      return Result::More;
    }
    const Result opened = openRead(sourcePaths, relocation.sourceEncrypted, relocation.source);
    if (opened == Result::Busy) return Result::Busy;
    if (opened == Result::Missing) {
      advanceRelocation(relocation);
      return Result::More;
    }
    if (opened != Result::Complete) {
      relocation.failedCaches++;
      advanceRelocation(relocation);
      return Result::More;
    }
    const Result sized = fileSize(relocation.source, relocation.sourceBytes);
    if (sized == Result::Busy) return Result::Busy;
    if (sized != Result::Complete || relocation.sourceBytes < (uint32_t)fingerprintAt + 8) {
      relocation.phase = Relocation::Phase::DiscardCache;
      return Result::More;
    }
    const Result writing = beginWrite(destinationPaths, relocation.destinationEncrypted,
                                      relocation.writer);
    if (writing == Result::Busy) return Result::Busy;
    if (writing != Result::Complete) {
      relocation.phase = Relocation::Phase::DiscardCache;
      return Result::More;
    }
    relocation.phase = Relocation::Phase::CopyCache;
    return Result::More;
  }

  if (relocation.phase == Relocation::Phase::CopyCache) {
    if (relocation.copiedBytes >= relocation.sourceBytes) {
      relocation.phase = Relocation::Phase::UpdateFingerprint;
      return Result::More;
    }
    const size_t remaining = relocation.sourceBytes - relocation.copiedBytes;
    const size_t amount = remaining < bufferBytes ? remaining : bufferBytes;
    const Result readResult = read(relocation.source, buffer, amount);
    if (readResult == Result::Busy) return Result::Busy;
    if (readResult != Result::Complete) {
      relocation.phase = Relocation::Phase::DiscardCache;
      return Result::More;
    }
    const Result writeResult = write(relocation.writer, buffer, amount);
    if (writeResult == Result::Busy) return Result::Busy;
    if (writeResult != Result::Complete) {
      relocation.phase = Relocation::Phase::DiscardCache;
      return Result::More;
    }
    relocation.copiedBytes += amount;
    return Result::More;
  }

  if (relocation.phase == Relocation::Phase::UpdateFingerprint) {
    uint8_t fingerprint[8];
    uint8_t offset = 0;
    writeLe32(fingerprint, relocation.destinationFingerprint.size);
    writeLe32(fingerprint + 4, relocation.destinationFingerprint.modified);
    if (!fingerprintOffset(format, offset)) {
      relocation.phase = Relocation::Phase::DiscardCache;
      return Result::More;
    }
    const Result seekResult = seek(relocation.writer, offset);
    if (seekResult == Result::Busy) return Result::Busy;
    if (seekResult != Result::Complete) {
      relocation.phase = Relocation::Phase::DiscardCache;
      return Result::More;
    }
    const Result writeResult = write(relocation.writer, fingerprint, sizeof(fingerprint));
    if (writeResult == Result::Busy) return Result::Busy;
    if (writeResult != Result::Complete) {
      relocation.phase = Relocation::Phase::DiscardCache;
      return Result::More;
    }
    relocation.phase = Relocation::Phase::SealCache;
    return Result::More;
  }

  if (relocation.phase == Relocation::Phase::SealCache) {
    const Result sealed = seal(relocation.writer);
    if (sealed == Result::Busy) return Result::Busy;
    if (sealed != Result::Complete) {
      relocation.phase = Relocation::Phase::DiscardCache;
      return Result::More;
    }
    relocation.preparedFormats |= (uint8_t)(1u << relocation.nextFormat);
    // The temporary file remains in the destination cache directory until the
    // staged primary file has been promoted. It is published separately below.
    relocation.writer = Writer();
    relocation.phase = Relocation::Phase::CloseCache;
    return Result::More;
  }

  if (relocation.phase == Relocation::Phase::CloseCache) {
    const Result closed = close(relocation.source);
    if (closed == Result::Busy) return Result::Busy;
    if (closed != Result::Complete) relocation.failedCaches++;
    advanceRelocation(relocation);
    return Result::More;
  }

  // A bad or unwritable sidecar must not make the primary move fail. Abort
  // its temporary destination, close the reader, and proceed to the next one.
  if (relocation.writer.active) {
    const Result aborted = abort(relocation.writer);
    if (aborted == Result::Busy) return Result::Busy;
  }
  if (relocation.source) {
    const Result closed = close(relocation.source);
    if (closed == Result::Busy) return Result::Busy;
  }
  relocation.failedCaches++;
  advanceRelocation(relocation);
  return Result::More;
}

MediaCache::Result MediaCache::publishRelocationStep(Relocation &relocation) const {
  const uint8_t count = FileFormats::internalCacheCount();
  if (relocation.nextPublish >= count) return Result::Complete;
  const uint8_t bit = (uint8_t)(1u << relocation.nextPublish);
  if ((relocation.preparedFormats & bit) == 0) {
    relocation.nextPublish++;
    return Result::More;
  }
  CachePaths paths;
  const FileFormats::Format format = FileFormats::internalCacheAt(relocation.nextPublish);
  if (!CachePath::forSource(relocation.destinationPath, format, paths)) {
    relocation.preparedFormats &= (uint8_t)~bit;
    relocation.failedCaches++;
    relocation.nextPublish++;
    return Result::More;
  }
  if (!SpiBusLock::lock(pdMS_TO_TICKS(LockTimeoutMs))) return Result::Busy;
  bool ready = _storage.exists(paths.temporaryPath);
  if (ready && _storage.exists(paths.finalPath)) ready = _storage.remove(paths.finalPath);
  const bool published = ready && _storage.rename(paths.temporaryPath, paths.finalPath);
  if (!published && _storage.exists(paths.temporaryPath)) _storage.remove(paths.temporaryPath);
  SpiBusLock::unlock();
  relocation.preparedFormats &= (uint8_t)~bit;
  if (!published) relocation.failedCaches++;
  relocation.nextPublish++;
  return Result::More;
}

MediaCache::Result MediaCache::discardRelocationStep(Relocation &relocation) const {
  const uint8_t count = FileFormats::internalCacheCount();
  if (relocation.nextPublish >= count) return Result::Complete;
  const uint8_t bit = (uint8_t)(1u << relocation.nextPublish);
  if ((relocation.preparedFormats & bit) == 0) {
    relocation.nextPublish++;
    return Result::More;
  }
  CachePaths paths;
  const FileFormats::Format format = FileFormats::internalCacheAt(relocation.nextPublish);
  if (!CachePath::forSource(relocation.destinationPath, format, paths)) {
    relocation.preparedFormats &= (uint8_t)~bit;
    relocation.failedCaches++;
    relocation.nextPublish++;
    return Result::More;
  }
  if (!SpiBusLock::lock(pdMS_TO_TICKS(LockTimeoutMs))) return Result::Busy;
  const bool removed = !_storage.exists(paths.temporaryPath) || _storage.remove(paths.temporaryPath);
  SpiBusLock::unlock();
  relocation.preparedFormats &= (uint8_t)~bit;
  if (!removed) relocation.failedCaches++;
  relocation.nextPublish++;
  return Result::More;
}

void MediaCache::abandonRelocation(Relocation &relocation) const {
  // SD removal is already handling an interrupted primary move. Best-effort
  // cleanup leaves any unavailable temporary file for normal cache cleanup.
  if (relocation.writer.active) abort(relocation.writer);
  if (relocation.source) relocation.source.close();
  if (relocation.destination) relocation.destination.close();
  relocation = Relocation();
}
