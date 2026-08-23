#pragma once

#include <Arduino.h>
#include <time.h>

class WiFiController;

enum class TimeStatus : uint8_t {
  NotConnected,
  Available
};

class TimeController {
public:
  static const long UtcOffsetSeconds = 4L * 60L * 60L;

  explicit TimeController(WiFiController &wifi);

  void begin();
  void update(uint32_t nowMs);

  TimeStatus status() const;
  bool localTime(struct tm &out) const;

private:
  static const time_t MinimumValidEpoch = 1609459200; // 2021-01-01 UTC

  WiFiController &_wifi;
  bool _hasTime;
  bool _configured;
  bool _wasWifiConnected;
};
