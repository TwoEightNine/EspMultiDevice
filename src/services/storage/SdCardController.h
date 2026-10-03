#pragma once

#include <Arduino.h>

enum class SdCardEvent {
  None,
  Mounted,
  Removed,
  MountFailed
};

enum class SdCardOwner : uint8_t {
  None,
  Device,
  UsbHost
};

enum class SdThroughputBenchmarkPhase : uint8_t {
  Write,
  Read
};

struct SdThroughputBenchmarkResult {
  bool success = false;
  bool cancelled = false;
  bool cleanupComplete = false;
  bool verified = false;
  uint32_t transferBytes = 0;
  uint32_t writeElapsedMs = 0;
  uint32_t readElapsedMs = 0;
  char error[96] = {};
};

class SdThroughputBenchmarkObserver {
public:
  virtual ~SdThroughputBenchmarkObserver() = default;
  virtual bool sdBenchmarkCancelled() const = 0;
  virtual void sdBenchmarkProgress(SdThroughputBenchmarkPhase phase,
                                   uint32_t completedBytes,
                                   uint32_t totalBytes) = 0;
};

class SdCardController {
public:
  SdCardController(int sckPin,
                   int misoPin,
                   int mosiPin,
                   int csPin,
                   int cardDetectPin = -1,
                   bool cardDetectActiveLow = true);

  bool begin();
  // Poll from the main loop so card state is kept current regardless of the
  // currently visible screen. Call takeEvent() from the active SD consumer.
  void update();
  SdCardEvent takeEvent();

  bool isMounted() const;
  bool isCardPresent() const;
  SdCardOwner owner() const;

  // Transfers exclusive ownership of the physical card from the local FAT
  // filesystem to a raw block device suitable for USB MSC. No filesystem
  // operation may be attempted until the device restarts or USB export ends.
  bool beginUsbExport();
  void endUsbExport();
  bool isUsbExportActive() const;
  uint32_t usbBlockCount() const;
  uint16_t usbBlockSize() const;
  int32_t readUsbBlocks(uint32_t lba, uint32_t offset,
                        void *buffer, uint32_t byteCount);
  int32_t writeUsbBlocks(uint32_t lba, uint32_t offset,
                         const uint8_t *buffer, uint32_t byteCount);

  bool runReadWriteTest(const char *path = "/sd_rw_test.txt");
  // Development-only diagnostic. It creates, verifies, and removes a
  // temporary file while reporting serial read/write throughput.
  bool runThroughputBenchmark();
  // Cancellable form used by worker-backed UI. The same verified 4 MiB test
  // and temporary-file cleanup contract applies; callbacks run on the caller.
  bool runThroughputBenchmark(SdThroughputBenchmarkResult &result,
                              SdThroughputBenchmarkObserver *observer);

private:
  // Arduino-ESP32's SPI SD backend clamps non-UHS cards to 25 MHz. Request
  // that effective rate explicitly so diagnostics and source agree.
  static const uint32_t SpiFrequency = 25000000;
  static const uint32_t BenchmarkBytes = 4UL * 1024UL * 1024UL;
  static const size_t BenchmarkChunkBytes = 4096;
  static const uint32_t BenchmarkLockTimeoutMs = 100;
  static const uint32_t MountRetryIntervalMs = 1000;
  // Without a physical CD pin the only way to find an inserted card is to
  // retry initialization. Keep this slow enough to avoid hammering an absent
  // or under-powered module, while still making a runtime reinsert usable.
  static const uint32_t NoDetectRetryIntervalMs = 3000;
  static const uint32_t HealthCheckIntervalMs = 500;

  int _sckPin;
  int _misoPin;
  int _mosiPin;
  int _csPin;
  int _cardDetectPin;
  bool _cardDetectActiveLow;
  bool _mounted;
  SdCardOwner _owner;
  uint8_t _rawDrive;
  uint32_t _rawBlockCount;
  uint16_t _rawBlockSize;
  uint32_t _lastMountAttemptMs;
  uint32_t _nextMountAttemptMs;
  uint32_t _lastHealthCheckMs;
  SdCardEvent _pendingEvent;

  bool mount();
  void unmount();
  bool mountedCardStillResponds();
  void configurePins();
  bool validateUsbRange(uint32_t lba, uint32_t offset,
                        uint32_t byteCount) const;
  bool readRawSector(uint32_t lba, uint8_t *buffer);
  bool writeRawSector(uint32_t lba, const uint8_t *buffer);

  bool writeTextFile(const char *path, const char *content);
  bool appendTextFile(const char *path, const char *content);
  bool readTextFile(const char *path, String &content);
};
