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
  SdCardEvent update();

  bool isMounted() const;
  bool isCardPresent() const;

  bool runReadWriteTest(const char *path = "/sd_rw_test.txt");
  bool writeTextFile(const char *path, const char *content);
  bool appendTextFile(const char *path, const char *content);
  bool readTextFile(const char *path, String &content);

private:
  static const uint32_t DefaultSpiFrequency = 4000000;
  static const uint32_t MountRetryIntervalMs = 1000;
  static const uint32_t HealthCheckIntervalMs = 500;

  int _sckPin;
  int _misoPin;
  int _mosiPin;
  int _csPin;
  int _cardDetectPin;
  bool _cardDetectActiveLow;
  bool _mounted;
  uint32_t _lastMountAttemptMs;
  uint32_t _lastHealthCheckMs;

  bool mount();
  void unmount();
  bool mountedCardStillResponds();
  void configurePins();
};
