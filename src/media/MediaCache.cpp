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
