#include "SdCardController.h"

#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <string.h>

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
  return _mounted;
}

bool SdCardController::isCardPresent() const {
  if (_cardDetectPin < 0) {
    return true;
  }

  int value = digitalRead(_cardDetectPin);
  return _cardDetectActiveLow ? value == LOW : value == HIGH;
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
  if (!_mounted) {
    Serial.println(F("SD throughput benchmark skipped: card is not mounted."));
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

  Serial.println(F("SD throughput benchmark: 4 MiB, 4 KiB SPI transactions."));

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
  return cleanupComplete;
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
    return false;
  }

  _mounted = true;
  _lastHealthCheckMs = millis();
  return true;
}

void SdCardController::unmount() {
  SD.end();
  _mounted = false;
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
