#include "SdCardController.h"

#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <string.h>

#include <sd_diskio.h>
extern "C" {
#include <diskio.h>
}

#include "../../core/SpiBusLock.h"

namespace {
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

bool SdCardController::readRawSector(uint32_t lba, uint8_t *buffer) {
  if (!buffer || !isUsbExportActive() || lba >= _rawBlockCount ||
      !SpiBusLock::lock(pdMS_TO_TICKS(1000))) {
    return false;
  }
  const bool read = sd_read_raw(_rawDrive, buffer, lba);
  SpiBusLock::unlock();
  return read;
}

bool SdCardController::writeRawSector(uint32_t lba, const uint8_t *buffer) {
  if (!buffer || !isUsbExportActive() || lba >= _rawBlockCount ||
      !SpiBusLock::lock(pdMS_TO_TICKS(1000))) {
    return false;
  }
  const bool written = sd_write_raw(_rawDrive, const_cast<uint8_t *>(buffer), lba);
  SpiBusLock::unlock();
  return written;
}

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
    const uint32_t available = _rawBlockSize - offset;
    const uint32_t chunk = min(available, byteCount - completed);
    if (offset == 0 && chunk == _rawBlockSize) {
      if (!readRawSector(lba, destination + completed)) return -1;
    } else {
      if (!readRawSector(lba, sector)) return -1;
      memcpy(destination + completed, sector + offset, chunk);
    }
    completed += chunk;
    offset = 0;
    lba++;
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
    const uint32_t available = _rawBlockSize - offset;
    const uint32_t chunk = min(available, byteCount - completed);
    if (offset == 0 && chunk == _rawBlockSize) {
      if (!writeRawSector(lba, buffer + completed)) return -1;
    } else {
      if (!readRawSector(lba, sector)) return -1;
      memcpy(sector + offset, buffer + completed, chunk);
      if (!writeRawSector(lba, sector)) return -1;
    }
    completed += chunk;
    offset = 0;
    lba++;
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

  Serial.println(F("SD throughput benchmark: 4 MiB, 4 KiB SPI transactions."));

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

  writeStartedMs = millis();
  for (uint32_t offset = 0; offset < BenchmarkBytes; offset += BenchmarkChunkBytes) {
    if (cancelled()) {
      result.cancelled = true;
      failure = F("cancelled");
      goto cleanup;
    }
    for (size_t i = 0; i < BenchmarkChunkBytes; i++) {
      buffer[i] = benchmarkByte(offset + i);
    }
    writeChecksum = updateBenchmarkChecksum(writeChecksum, buffer, BenchmarkChunkBytes);

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
    for (size_t i = 0; i < BenchmarkChunkBytes; i++) {
      if (buffer[i] != benchmarkByte(offset + i)) {
        failure = F("data verification failed");
        goto cleanup;
      }
    }
    readChecksum = updateBenchmarkChecksum(readChecksum, buffer, BenchmarkChunkBytes);
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

  if (readChecksum != writeChecksum) {
    failure = F("checksum verification failed");
    goto cleanup;
  }
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

  SD.end();
  SPI.end();
  delay(10);
  yield();

  configurePins();
  SPI.begin(_sckPin, _misoPin, _mosiPin, _csPin);

  if (!SD.begin(_csPin, SPI, SpiFrequency)) {
    SD.end();
    _mounted = false;
    if (_cardDetectPin < 0) {
      _nextMountAttemptMs = millis() + NoDetectRetryIntervalMs;
      Serial.println(F("SD mount failed; retrying in 3 seconds without a card-detect pin."));
    }
    SpiBusLock::unlock();
    return false;
  }
  Serial.println(F("SD mounted at 25 MHz SPI."));

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

  _mounted = true;
  _owner = SdCardOwner::Device;
  _lastHealthCheckMs = millis();
  SpiBusLock::unlock();
  return true;
}

void SdCardController::unmount() {
  const bool locked = SpiBusLock::lock(portMAX_DELAY);
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
