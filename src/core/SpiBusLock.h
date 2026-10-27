#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// The SD card and XPT2046 share the same SPI wires. GIF cache reads can run
// on the other core, so both users must serialize individual transactions.
class SpiBusLock {
public:
  static bool begin();
  static bool lock(TickType_t timeout = portMAX_DELAY);
  static void unlock();
  // Name of the task holding the bus, or "none"; for failure diagnostics.
  static const char *holderName();

private:
  static SemaphoreHandle_t _mutex;
};
