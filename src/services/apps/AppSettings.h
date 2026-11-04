#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "AppDisabledList.h"

// Per-device enable/disable state for SD apps, persisted in NVS so it stays
// with this device across card swaps. Only disabled IDs are stored; every
// other app, including one installed later, is enabled. Thread-safe: Home's
// catalog worker and app workers read it while Settings writes it.
class AppSettings {
public:
  AppSettings();

  void begin();
  bool isEnabled(const char *id) const;
  bool isEnabled(const String &id) const { return isEnabled(id.c_str()); }
  // Returns true once the change is committed (or nothing needed changing).
  bool setEnabled(const char *id, bool enabled);

private:
  SemaphoreHandle_t _mutex = nullptr;
  AppDisabledList _disabled;

  bool save(const AppDisabledList &list);
};
