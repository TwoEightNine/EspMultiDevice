#pragma once

#include <Arduino.h>

enum class SdCardEvent {
  None,
  Mounted,
  Removed,
  MountFailed
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

  bool runReadWriteTest(const char *path = "/sd_rw_test.txt");

private:
  // Long sequential animation-cache reads exposed marginal signal integrity
  // on otherwise mountable cards at 40 MHz. Keep the bus at the ESP32 SD
  // library's conservative, proven rate for reliable playback.
  static const uint32_t SpiFrequency = 40000000;
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
  uint32_t _lastMountAttemptMs;
  uint32_t _nextMountAttemptMs;
  uint32_t _lastHealthCheckMs;
  SdCardEvent _pendingEvent;

  bool mount();
  void unmount();
  bool mountedCardStillResponds();
  void configurePins();

  bool writeTextFile(const char *path, const char *content);
  bool appendTextFile(const char *path, const char *content);
  bool readTextFile(const char *path, String &content);
};
