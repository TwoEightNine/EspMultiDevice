#include "SdCardController.h"

#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <sd_diskio.h>
extern "C" {
#include <diskio.h>
}

#include "../../core/SpiBusLock.h"
#include "SdSpiDriver.h"

namespace {
// SDFS keeps its FatFs drive number protected. A member pointer formed
// through a derived class reads it without modifying the core library.
struct SdDrive : fs::SDFS {
  static uint8_t of(fs::SDFS &sd) { return sd.*(&SdDrive::_pdrv); }
};

const char *BenchmarkDirectoryPath = "/.system";
const char *BenchmarkPath = "/.system/sd_benchmark.tmp";

uint8_t benchmarkByte(uint32_t offset) {
  return static_cast<uint8_t>((offset * 37UL + 17UL) ^ (offset >> 5));
}

uint32_t updateBenchmarkChecksum(uint32_t checksum, const uint8_t *bytes, size_t length) {
  for (size_t i = 0; i < length; i++) {
    checksum ^= bytes[i];
    checksum *= 16777619UL;
  }
  return checksum;
}

// Development breakdown of one benchmark phase: driver bus time per stage
// versus the bare wire time of the sectors moved, plus benchmark CPU work.
void printDriverStats(const char *phase, uint32_t elapsedMs, uint32_t cpuUs) {
  const SdSpiDriver::Stats stats = SdSpiDriver::stats();
  const uint32_t clockHz = SdSpiDriver::info().clockHz;
  // Each block is 512 data bytes plus a 2-byte CRC on the wire.
  auto wireUs = [clockHz](uint32_t sectors) -> unsigned long {
    return clockHz ? static_cast<unsigned long>(
                         static_cast<uint64_t>(sectors) * 514ULL * 8ULL * 1000000ULL / clockHz)
                   : 0UL;
  };
  Serial.printf("SD %s: elapsed=%lu ms, benchmark cpu=%lu ms\n", phase,
                static_cast<unsigned long>(elapsedMs),
                static_cast<unsigned long>(cpuUs / 1000));
  Serial.printf("  read: calls=%lu sectors=%lu max/call=%lu bus=%lu us "
                "(access=%lu gap=%lu data=%lu wire=%lu stop=%lu)\n",
                static_cast<unsigned long>(stats.readCalls),
                static_cast<unsigned long>(stats.readSectors),
                static_cast<unsigned long>(stats.maxReadSectors),
                static_cast<unsigned long>(stats.readUs),
                static_cast<unsigned long>(stats.accessUs),
                static_cast<unsigned long>(stats.gapUs),
                static_cast<unsigned long>(stats.dataUs),
                wireUs(stats.readSectors),
                static_cast<unsigned long>(stats.stopUs));
  Serial.printf("  write: calls=%lu sectors=%lu max/call=%lu bus=%lu us (busy=%lu wire=%lu)\n",
                static_cast<unsigned long>(stats.writeCalls),
                static_cast<unsigned long>(stats.writeSectors),
                static_cast<unsigned long>(stats.maxWriteSectors),
                static_cast<unsigned long>(stats.writeUs),
                static_cast<unsigned long>(stats.writeBusyUs),
                wireUs(stats.writeSectors));
}

float mebibytesPerSecond(uint32_t bytes, uint32_t elapsedMs) {
  if (elapsedMs == 0) elapsedMs = 1;
  return (static_cast<float>(bytes) * 1000.0f) /
         (1024.0f * 1024.0f * static_cast<float>(elapsedMs));
}
}

SdCardController::SdCardController(int sckPin,
                                   int misoPin,
                                   int mosiPin,
                                   int csPin,
                                   int cardDetectPin,
                                   bool cardDetectActiveLow)
  : _sckPin(sckPin),
    _misoPin(misoPin),
    _mosiPin(mosiPin),
    _csPin(csPin),
    _cardDetectPin(cardDetectPin),
    _cardDetectActiveLow(cardDetectActiveLow),
    _mounted(false),
    _owner(SdCardOwner::None),
    _rawDrive(0xff),
    _rawBlockCount(0),
    _rawBlockSize(0),
    _lastMountAttemptMs(0),
    _nextMountAttemptMs(0),
    _lastHealthCheckMs(0),
    _pendingEvent(SdCardEvent::None) {
}

bool SdCardController::begin() {
  configurePins();
  return mount();
}

void SdCardController::update() {
  if (_owner == SdCardOwner::UsbHost) {
    return;
  }

  uint32_t now = millis();
  bool present = isCardPresent();

  if (!present) {
    if (_mounted) {
      unmount();
      _pendingEvent = SdCardEvent::Removed;
      return;
    }

    return;
  }

  if (_mounted && now - _lastHealthCheckMs >= HealthCheckIntervalMs) {
    _lastHealthCheckMs = now;
    if (!mountedCardStillResponds()) {
      unmount();
      _pendingEvent = SdCardEvent::Removed;
      return;
    }
  }

  if (!_mounted && now >= _nextMountAttemptMs && now - _lastMountAttemptMs >= MountRetryIntervalMs) {
    if (mount()) {
      _pendingEvent = SdCardEvent::Mounted;
      return;
    }

    _pendingEvent = SdCardEvent::MountFailed;
    return;
  }
}

SdCardEvent SdCardController::takeEvent() {
  SdCardEvent event = _pendingEvent;
  _pendingEvent = SdCardEvent::None;
  return event;
}

bool SdCardController::isMounted() const {
  return _mounted && _owner == SdCardOwner::Device;
}

bool SdCardController::isCardPresent() const {
  if (_cardDetectPin < 0) {
    return true;
  }

  int value = digitalRead(_cardDetectPin);
  return _cardDetectActiveLow ? value == LOW : value == HIGH;
}

SdCardOwner SdCardController::owner() const {
  return _owner;
}

bool SdCardController::beginUsbExport() {
  if (_owner == SdCardOwner::UsbHost) {
    return _rawDrive != 0xff && _rawBlockCount > 0 && _rawBlockSize == 512;
  }
  if (!_mounted || _owner != SdCardOwner::Device) {
    return false;
  }
  if (!SpiBusLock::lock(pdMS_TO_TICKS(2000))) {
    return false;
  }

  // Drop every FatFs/VFS structure before the host can access sector zero.
  // The raw driver is then initialized without mounting a local filesystem,
  // so there can be only one FAT owner during the USB session.
  SdSpiDriver::detach();
  SD.end();
  _mounted = false;
  _owner = SdCardOwner::None;
  _pendingEvent = SdCardEvent::None;

  SPI.end();
  delay(10);
  configurePins();
  SPI.begin(_sckPin, _misoPin, _mosiPin, _csPin);

  _rawDrive = sdcard_init(_csPin, &SPI, SpiFrequency);
  bool ready = _rawDrive != 0xff &&
               (disk_initialize(_rawDrive) & STA_NOINIT) == 0;
  if (ready) {
    _rawBlockCount = sdcard_num_sectors(_rawDrive);
    _rawBlockSize = static_cast<uint16_t>(sdcard_sector_size(_rawDrive));
    ready = _rawBlockCount > 0 && _rawBlockSize == 512;
  }
  if (ready) attachFastDriver(_rawDrive);

  if (!ready) {
    if (_rawDrive != 0xff) {
      sdcard_uninit(_rawDrive);
    }
    _rawDrive = 0xff;
    _rawBlockCount = 0;
    _rawBlockSize = 0;
    SpiBusLock::unlock();
    return false;
  }

  _owner = SdCardOwner::UsbHost;
  SpiBusLock::unlock();
  return true;
}

void SdCardController::endUsbExport() {
  if (_rawDrive != 0xff) {
    if (SpiBusLock::lock(pdMS_TO_TICKS(2000))) {
      SdSpiDriver::detach();
      sdcard_uninit(_rawDrive);
      SpiBusLock::unlock();
    }
  }
  _rawDrive = 0xff;
  _rawBlockCount = 0;
  _rawBlockSize = 0;
  _mounted = false;
  _owner = SdCardOwner::None;
}

bool SdCardController::isUsbExportActive() const {
  return _owner == SdCardOwner::UsbHost && _rawDrive != 0xff;
}

uint32_t SdCardController::usbBlockCount() const {
  return isUsbExportActive() ? _rawBlockCount : 0;
}

uint16_t SdCardController::usbBlockSize() const {
  return isUsbExportActive() ? _rawBlockSize : 0;
}

bool SdCardController::validateUsbRange(uint32_t lba, uint32_t offset,
                                        uint32_t byteCount) const {
  if (!isUsbExportActive() || _rawBlockSize == 0 || byteCount == 0) {
    return false;
  }
  const uint64_t start = static_cast<uint64_t>(lba) * _rawBlockSize + offset;
  const uint64_t capacity = static_cast<uint64_t>(_rawBlockCount) * _rawBlockSize;
  return start < capacity && byteCount <= capacity - start;
}

bool SdCardController::readRawSectors(uint32_t lba, uint8_t *buffer,
                                      uint32_t count) {
  if (!buffer || !count || !isUsbExportActive() || lba >= _rawBlockCount ||
      count > _rawBlockCount - lba || !SpiBusLock::lock(pdMS_TO_TICKS(1000))) {
    return false;
  }
  const bool read = disk_read(_rawDrive, buffer, lba, count) == RES_OK;
  SpiBusLock::unlock();
  return read;
}

bool SdCardController::writeRawSectors(uint32_t lba, const uint8_t *buffer,
                                       uint32_t count) {
  if (!buffer || !count || !isUsbExportActive() || lba >= _rawBlockCount ||
      count > _rawBlockCount - lba || !SpiBusLock::lock(pdMS_TO_TICKS(1000))) {
    return false;
  }
  const bool written = disk_write(_rawDrive, buffer, lba, count) == RES_OK;
  SpiBusLock::unlock();
  return written;
}

// Whole-sector runs move as one multi-block command; only a partial head or
// tail sector goes through the read-modify-write bounce buffer.
int32_t SdCardController::readUsbBlocks(uint32_t lba, uint32_t offset,
                                        void *buffer, uint32_t byteCount) {
  if (!buffer || !validateUsbRange(lba, offset, byteCount)) {
    return -1;
  }

  uint8_t *destination = static_cast<uint8_t *>(buffer);
  uint8_t sector[512];
  lba += offset / _rawBlockSize;
  offset %= _rawBlockSize;
  uint32_t completed = 0;
  while (completed < byteCount) {
    const uint32_t remaining = byteCount - completed;
    if (offset == 0 && remaining >= _rawBlockSize) {
      const uint32_t sectors = min(remaining / _rawBlockSize, MaxUsbBurstSectors);
      if (!readRawSectors(lba, destination + completed, sectors)) return -1;
      completed += sectors * _rawBlockSize;
      lba += sectors;
    } else {
      const uint32_t chunk = min(_rawBlockSize - offset, remaining);
      if (!readRawSectors(lba, sector, 1)) return -1;
      memcpy(destination + completed, sector + offset, chunk);
      completed += chunk;
      offset = 0;
      lba++;
    }
    taskYIELD();
  }
  return static_cast<int32_t>(completed);
}

int32_t SdCardController::writeUsbBlocks(uint32_t lba, uint32_t offset,
                                         const uint8_t *buffer, uint32_t byteCount) {
  if (!buffer || !validateUsbRange(lba, offset, byteCount)) {
    return -1;
  }

  uint8_t sector[512];
  lba += offset / _rawBlockSize;
  offset %= _rawBlockSize;
  uint32_t completed = 0;
  while (completed < byteCount) {
    const uint32_t remaining = byteCount - completed;
    if (offset == 0 && remaining >= _rawBlockSize) {
      const uint32_t sectors = min(remaining / _rawBlockSize, MaxUsbBurstSectors);
      if (!writeRawSectors(lba, buffer + completed, sectors)) return -1;
      completed += sectors * _rawBlockSize;
      lba += sectors;
    } else {
      const uint32_t chunk = min(_rawBlockSize - offset, remaining);
      if (!readRawSectors(lba, sector, 1)) return -1;
      memcpy(sector + offset, buffer + completed, chunk);
      if (!writeRawSectors(lba, sector, 1)) return -1;
      completed += chunk;
      offset = 0;
      lba++;
    }
    taskYIELD();
  }
  return static_cast<int32_t>(completed);
}

bool SdCardController::runReadWriteTest(const char *path) {
  if (!_mounted) {
    return false;
  }

  char firstLine[96];
  char secondLine[96];
  snprintf(firstLine, sizeof(firstLine), "ESP Media SD write test, millis=%lu\n", (unsigned long)millis());
  snprintf(secondLine, sizeof(secondLine), "ESP Media SD append test, millis=%lu\n", (unsigned long)millis());

  if (!writeTextFile(path, firstLine)) {
    return false;
  }

  if (!appendTextFile(path, secondLine)) {
    return false;
  }

  String content;
  if (!readTextFile(path, content)) {
    return false;
  }

  return content.indexOf(firstLine) >= 0 && content.indexOf(secondLine) >= 0;
}

bool SdCardController::runThroughputBenchmark() {
  SdThroughputBenchmarkResult result;
  return runThroughputBenchmark(result, nullptr);
}

bool SdCardController::runThroughputBenchmark(
    SdThroughputBenchmarkResult &result,
    SdThroughputBenchmarkObserver *observer) {
  result = {};
  result.transferBytes = BenchmarkBytes;
  if (!_mounted) {
    Serial.println(F("SD throughput benchmark skipped: card is not mounted."));
    strlcpy(result.error, "SD card is not mounted", sizeof(result.error));
    return false;
  }

  bool directoryExisted = false;
  bool createdDirectory = false;
  bool success = false;
  File file;
  static uint8_t buffer[BenchmarkChunkBytes];
  uint32_t writeChecksum = 2166136261UL;
  uint32_t readChecksum = 2166136261UL;
  uint32_t writeElapsedMs = 0;
  uint32_t readElapsedMs = 0;
  uint32_t bulkReadElapsedMs = 0;
  uint32_t writeCpuUs = 0;
  uint32_t readCpuUs = 0;
  uint32_t bulkReadCpuUs = 0;
  uint32_t writeStartedMs = 0;
  uint32_t readStartedMs = 0;
  const __FlashStringHelper *failure = nullptr;

  auto cancelled = [&]() {
    return observer && observer->sdBenchmarkCancelled();
  };
  auto reportProgress = [&](SdThroughputBenchmarkPhase phase,
                            uint32_t completedBytes) {
    if (observer) observer->sdBenchmarkProgress(phase, completedBytes,
                                                BenchmarkBytes);
  };
  // Re-reads the file through an unbuffered descriptor in large requests,
  // which reach FatFs as multi-sector reads instead of 4 KiB stdio refills.
  auto bulkRead = [&]() -> bool {
    uint8_t *bulk = static_cast<uint8_t *>(ps_malloc(BulkReadChunkBytes));
    if (!bulk) {
      failure = F("no memory for bulk read buffer");
      return false;
    }
    const String path = String(MountPoint) + BenchmarkPath;
    int fd = -1;
    if (SpiBusLock::lock(pdMS_TO_TICKS(BenchmarkLockTimeoutMs))) {
      fd = ::open(path.c_str(), O_RDONLY);
      SpiBusLock::unlock();
      if (fd < 0) failure = F("could not open benchmark file for bulk read");
    } else {
      failure = F("SPI bus busy while opening bulk read");
    }
    SdSpiDriver::resetStats();
    const uint32_t startedMs = millis();
    uint32_t offset = 0;
    while (fd >= 0 && offset < BenchmarkBytes) {
      if (cancelled()) {
        result.cancelled = true;
        failure = F("cancelled");
        break;
      }
      if (!SpiBusLock::lock(pdMS_TO_TICKS(BenchmarkLockTimeoutMs))) {
        failure = F("SPI bus busy during bulk read");
        break;
      }
      const ssize_t read = ::read(fd, bulk, BulkReadChunkBytes);
      SpiBusLock::unlock();
      if (read != static_cast<ssize_t>(BulkReadChunkBytes)) {
        failure = F("short bulk read during benchmark");
        break;
      }
      const uint32_t verifyStartedUs = micros();
      bool matches = true;
      for (size_t i = 0; matches && i < BulkReadChunkBytes; i++) {
        matches = bulk[i] == benchmarkByte(offset + i);
      }
      bulkReadCpuUs += micros() - verifyStartedUs;
      if (!matches) {
        failure = F("bulk read verification failed");
        break;
      }
      offset += BulkReadChunkBytes;
      reportProgress(SdThroughputBenchmarkPhase::BulkRead, offset);
      yield();
    }
    bulkReadElapsedMs = millis() - startedMs;
    printDriverStats("bulk read", bulkReadElapsedMs, bulkReadCpuUs);
    if (fd >= 0 && SpiBusLock::lock(pdMS_TO_TICKS(BenchmarkLockTimeoutMs))) {
      ::close(fd);
      SpiBusLock::unlock();
    }
    free(bulk);
    return offset == BenchmarkBytes;
  };

  Serial.println(F("SD throughput benchmark: 4 MiB in 4 KiB file transactions, then 32 KiB bulk reads."));

  if (cancelled()) {
    result.cancelled = true;
    failure = F("cancelled");
    goto cleanup;
  }

  if (!SpiBusLock::lock(pdMS_TO_TICKS(BenchmarkLockTimeoutMs))) {
    failure = F("SPI bus busy while checking benchmark directory");
    goto cleanup;
  }
  directoryExisted = SD.exists(BenchmarkDirectoryPath);
  SpiBusLock::unlock();

  if (!directoryExisted) {
    if (!SpiBusLock::lock(pdMS_TO_TICKS(BenchmarkLockTimeoutMs))) {
      failure = F("SPI bus busy while creating benchmark directory");
      goto cleanup;
    }
    createdDirectory = SD.mkdir(BenchmarkDirectoryPath);
    SpiBusLock::unlock();
    if (!createdDirectory) {
      failure = F("could not create benchmark directory");
      goto cleanup;
    }
  }

  if (!SpiBusLock::lock(pdMS_TO_TICKS(BenchmarkLockTimeoutMs))) {
    failure = F("SPI bus busy while opening benchmark file for writing");
    goto cleanup;
  }
  SD.remove(BenchmarkPath);
  file = SD.open(BenchmarkPath, FILE_WRITE);
  SpiBusLock::unlock();
  if (!file) {
    failure = F("could not open benchmark file for writing");
    goto cleanup;
  }

  SdSpiDriver::resetStats();
  writeStartedMs = millis();
  for (uint32_t offset = 0; offset < BenchmarkBytes; offset += BenchmarkChunkBytes) {
    if (cancelled()) {
      result.cancelled = true;
      failure = F("cancelled");
      goto cleanup;
    }
    const uint32_t prepareStartedUs = micros();
    for (size_t i = 0; i < BenchmarkChunkBytes; i++) {
      buffer[i] = benchmarkByte(offset + i);
    }
    writeChecksum = updateBenchmarkChecksum(writeChecksum, buffer, BenchmarkChunkBytes);
    writeCpuUs += micros() - prepareStartedUs;

    if (!SpiBusLock::lock(pdMS_TO_TICKS(BenchmarkLockTimeoutMs))) {
      failure = F("SPI bus busy during benchmark write");
      goto cleanup;
    }
    size_t written = file.write(buffer, BenchmarkChunkBytes);
    SpiBusLock::unlock();
    if (written != BenchmarkChunkBytes) {
      failure = F("short write during benchmark");
      goto cleanup;
    }
    reportProgress(SdThroughputBenchmarkPhase::Write,
                   offset + BenchmarkChunkBytes);
    yield();
  }
  if (!SpiBusLock::lock(pdMS_TO_TICKS(BenchmarkLockTimeoutMs))) {
    failure = F("SPI bus busy while flushing benchmark write");
    goto cleanup;
  }
  file.flush();
  file.close();
  SpiBusLock::unlock();
  writeElapsedMs = millis() - writeStartedMs;
  printDriverStats("write", writeElapsedMs, writeCpuUs);

  if (!SpiBusLock::lock(pdMS_TO_TICKS(BenchmarkLockTimeoutMs))) {
    failure = F("SPI bus busy while opening benchmark file for reading");
    goto cleanup;
  }
  file = SD.open(BenchmarkPath, FILE_READ);
  SpiBusLock::unlock();
  if (!file) {
    failure = F("could not open benchmark file for reading");
    goto cleanup;
  }

  SdSpiDriver::resetStats();
  readStartedMs = millis();
  for (uint32_t offset = 0; offset < BenchmarkBytes; offset += BenchmarkChunkBytes) {
    if (cancelled()) {
      result.cancelled = true;
      failure = F("cancelled");
      goto cleanup;
    }
    if (!SpiBusLock::lock(pdMS_TO_TICKS(BenchmarkLockTimeoutMs))) {
      failure = F("SPI bus busy during benchmark read");
      goto cleanup;
    }
    size_t read = file.read(buffer, BenchmarkChunkBytes);
    SpiBusLock::unlock();
    if (read != BenchmarkChunkBytes) {
      failure = F("short read during benchmark");
      goto cleanup;
    }
    const uint32_t verifyStartedUs = micros();
    for (size_t i = 0; i < BenchmarkChunkBytes; i++) {
      if (buffer[i] != benchmarkByte(offset + i)) {
        failure = F("data verification failed");
        goto cleanup;
      }
    }
    readChecksum = updateBenchmarkChecksum(readChecksum, buffer, BenchmarkChunkBytes);
    readCpuUs += micros() - verifyStartedUs;
    reportProgress(SdThroughputBenchmarkPhase::Read,
                   offset + BenchmarkChunkBytes);
    yield();
  }
  if (!SpiBusLock::lock(pdMS_TO_TICKS(BenchmarkLockTimeoutMs))) {
    failure = F("SPI bus busy while closing benchmark read");
    goto cleanup;
  }
  file.close();
  SpiBusLock::unlock();
  readElapsedMs = millis() - readStartedMs;
  printDriverStats("read", readElapsedMs, readCpuUs);

  if (readChecksum != writeChecksum) {
    failure = F("checksum verification failed");
    goto cleanup;
  }
  if (!bulkRead()) goto cleanup;
  success = true;
  result.verified = true;

cleanup:
  if (file) {
    if (SpiBusLock::lock(pdMS_TO_TICKS(BenchmarkLockTimeoutMs))) {
      file.close();
      SpiBusLock::unlock();
    }
  }

  bool cleanupComplete = false;
  if (SpiBusLock::lock(pdMS_TO_TICKS(BenchmarkLockTimeoutMs))) {
    bool removedFile = !SD.exists(BenchmarkPath) || SD.remove(BenchmarkPath);
    bool removedDirectory = !createdDirectory || SD.rmdir(BenchmarkDirectoryPath);
    cleanupComplete = removedFile && removedDirectory;
    SpiBusLock::unlock();
  }

  result.cleanupComplete = cleanupComplete;
  result.writeElapsedMs = writeElapsedMs;
  result.readElapsedMs = readElapsedMs;
  result.bulkReadElapsedMs = bulkReadElapsedMs;
  result.clockHz = SdSpiDriver::info().clockHz;
  result.success = success && cleanupComplete;
  if (failure) {
    String error(failure);
    strlcpy(result.error, error.c_str(), sizeof(result.error));
  } else if (!cleanupComplete) {
    strlcpy(result.error, "Temporary-file cleanup failed", sizeof(result.error));
  }

  if (!success) {
    Serial.print(F("SD throughput benchmark failed: "));
    Serial.println(failure ? failure : F("unknown error"));
    if (!cleanupComplete) Serial.println(F("SD throughput benchmark cleanup incomplete."));
    return false;
  }

  Serial.print(F("SD benchmark write: elapsedMs="));
  Serial.print(writeElapsedMs);
  Serial.print(F(", MiB/s="));
  Serial.println(mebibytesPerSecond(BenchmarkBytes, writeElapsedMs), 2);
  Serial.print(F("SD benchmark read: elapsedMs="));
  Serial.print(readElapsedMs);
  Serial.print(F(", MiB/s="));
  Serial.println(mebibytesPerSecond(BenchmarkBytes, readElapsedMs), 2);
  Serial.print(F("SD benchmark bulk read: elapsedMs="));
  Serial.print(bulkReadElapsedMs);
  Serial.print(F(", MiB/s="));
  Serial.println(mebibytesPerSecond(BenchmarkBytes, bulkReadElapsedMs), 2);
  Serial.print(F("SD benchmark clock: "));
  Serial.print(result.clockHz / 1000000.0f, 2);
  Serial.print(F(" MHz, DMA "));
  Serial.print(SdSpiDriver::info().dma ? F("on") : F("off"));
  if (SdSpiDriver::info().dmaFaults) {
    Serial.print(F(", DMA faults="));
    Serial.print(SdSpiDriver::info().dmaFaults);
  }
  if (SdSpiDriver::info().dmaFault[0]) {
    Serial.print(F(" [last: "));
    Serial.print(SdSpiDriver::info().dmaFault);
    Serial.print(']');
  }
  Serial.print(F(", retries="));
  Serial.print(SdSpiDriver::info().retries);
  Serial.print(F(", step-downs="));
  Serial.println(SdSpiDriver::info().stepDowns);
  Serial.println(cleanupComplete ? F("SD throughput benchmark passed; temporary file removed.")
                                 : F("SD throughput benchmark passed, but temporary-file cleanup was incomplete."));
  return result.success;
}

bool SdCardController::writeTextFile(const char *path, const char *content) {
  if (!_mounted) {
    return false;
  }

  File file = SD.open(path, FILE_WRITE);
  if (!file) {
    unmount();
    return false;
  }

  size_t expected = strlen(content);
  size_t written = file.print(content);
  file.close();

  if (written != expected) {
    unmount();
    return false;
  }

  return true;
}

bool SdCardController::appendTextFile(const char *path, const char *content) {
  if (!_mounted) {
    return false;
  }

  File file = SD.open(path, FILE_APPEND);
  if (!file) {
    unmount();
    return false;
  }

  size_t expected = strlen(content);
  size_t written = file.print(content);
  file.close();

  if (written != expected) {
    unmount();
    return false;
  }

  return true;
}

bool SdCardController::readTextFile(const char *path, String &content) {
  content = "";

  if (!_mounted) {
    return false;
  }

  File file = SD.open(path, FILE_READ);
  if (!file) {
    unmount();
    return false;
  }

  while (file.available()) {
    content += (char)file.read();
  }

  file.close();
  return true;
}

bool SdCardController::mount() {
  _lastMountAttemptMs = millis();
  _nextMountAttemptMs = _lastMountAttemptMs +
                         (_cardDetectPin < 0 ? NoDetectRetryIntervalMs : MountRetryIntervalMs);

  if (!isCardPresent()) {
    return false;
  }

  // A worker may have a file open when the card is removed/reinserted. Never
  // reset the SPI and FatFs drivers in the middle of its bounded transaction.
  if (!SpiBusLock::lock(pdMS_TO_TICKS(200))) return false;

  SdSpiDriver::detach();
  SD.end();
  SPI.end();
  delay(10);
  yield();

  configurePins();
  SPI.begin(_sckPin, _misoPin, _mosiPin, _csPin);

  if (!SD.begin(_csPin, SPI, SpiFrequency, MountPoint, MaxOpenFiles)) {
    SD.end();
    _mounted = false;
    if (_cardDetectPin < 0) {
      _nextMountAttemptMs = millis() + NoDetectRetryIntervalMs;
      Serial.println(F("SD mount failed; retrying in 3 seconds without a card-detect pin."));
    }
    SpiBusLock::unlock();
    return false;
  }
  if (SD.cardType() == CARD_NONE) {
    SD.end();
    _mounted = false;
    if (_cardDetectPin < 0) {
      _nextMountAttemptMs = millis() + NoDetectRetryIntervalMs;
      Serial.println(F("SD card not detected; retrying in 3 seconds without a card-detect pin."));
    }
    SpiBusLock::unlock();
    return false;
  }

  attachFastDriver(SdDrive::of(SD));
  _mounted = true;
  _owner = SdCardOwner::Device;
  _lastHealthCheckMs = millis();
  SpiBusLock::unlock();
  return true;
}

// Called with SpiBusLock held after the backend initialized the card. When
// attach fails the backend's own driver remains registered and functional.
void SdCardController::attachFastDriver(uint8_t drive) {
  if (!SdSpiDriver::attach(drive, static_cast<uint8_t>(_csPin),
                           sdcard_type(drive) == CARD_SDHC,
                           sdcard_num_sectors(drive))) {
    Serial.println(F("SD SPI driver unavailable; using the 20 MHz backend driver."));
  }
}

void SdCardController::unmount() {
  const bool locked = SpiBusLock::lock(portMAX_DELAY);
  SdSpiDriver::detach();
  SD.end();
  _mounted = false;
  _owner = SdCardOwner::None;
  if (locked) SpiBusLock::unlock();
}

bool SdCardController::mountedCardStillResponds() {
  if (!_mounted) {
    return false;
  }

  // Opening the root directory only consults the mounted VFS and can still
  // succeed after a card has been removed. Reading sector zero forces an SPI
  // transaction, which reliably detects an ejected card when no CD pin is
  // available. Defer rather than report a removal while another SPI user has
  // the shared bus.
  if (!SpiBusLock::lock(pdMS_TO_TICKS(20))) {
    return true;
  }

  uint8_t sector[512];
  bool ok = SD.sectorSize() == sizeof(sector) && SD.numSectors() > 0 &&
            SD.readRAW(sector, 0);
  SpiBusLock::unlock();
  return ok;
}

void SdCardController::configurePins() {
  pinMode(_csPin, OUTPUT);
  digitalWrite(_csPin, HIGH);

  if (_cardDetectPin >= 0) {
    pinMode(_cardDetectPin, _cardDetectActiveLow ? INPUT_PULLUP : INPUT_PULLDOWN);
  }
}
