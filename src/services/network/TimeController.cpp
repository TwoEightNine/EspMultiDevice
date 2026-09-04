#include "TimeController.h"

#include "WiFiController.h"

namespace {
const char *NtpServer1 = "pool.ntp.org";
const char *NtpServer2 = "time.nist.gov";
}

TimeController::TimeController(WiFiController &wifi)
  : _wifi(wifi),
    _hasTime(false),
    _configured(false),
    _wasWifiConnected(false) {
}

void TimeController::begin() {
  _hasTime = false;
  _configured = false;
  _wasWifiConnected = false;
}

void TimeController::update(uint32_t nowMs) {
  (void)nowMs;

  const bool wifiConnected = _wifi.isConnected();
  if (!wifiConnected) {
    _wasWifiConnected = false;
    return;
  }

  // Re-arm SNTP whenever Wi-Fi becomes connected. configTime() only starts
  // the asynchronous request; it does not wait for a network response.
  if (!_wasWifiConnected || !_configured) {
    configTime(UtcOffsetSeconds, 0, NtpServer1, NtpServer2);
    _configured = true;
  }
  _wasWifiConnected = true;

  time_t current = time(nullptr);
  if (current >= MinimumValidEpoch) {
    _hasTime = true;
  }
}

TimeStatus TimeController::status() const {
  return _hasTime ? TimeStatus::Available : TimeStatus::NotConnected;
}

bool TimeController::localTime(struct tm &out) const {
  if (!_hasTime) {
    return false;
  }

  time_t current = time(nullptr);
  if (current < MinimumValidEpoch) {
    return false;
  }
  return localtime_r(&current, &out) != nullptr;
}
