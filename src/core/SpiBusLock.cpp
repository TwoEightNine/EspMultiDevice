#include "SpiBusLock.h"

#include <esp_timer.h>

SemaphoreHandle_t SpiBusLock::_mutex = nullptr;

namespace {
// Diagnostics: who holds the bus and since when. Written only by the holder.
// Battery history appends hold it for about 50 ms every second; report only
// holds well beyond that.
constexpr int64_t LongHoldUs = 100000;
constexpr uint32_t ReportIntervalMs = 250;
volatile int64_t lockedAtUs = 0;
volatile uint32_t lastWaitReportMs = 0;
volatile uint32_t lastHoldReportMs = 0;

bool reportAllowed(volatile uint32_t &lastReportMs) {
  const uint32_t now = millis();
  if (now - lastReportMs < ReportIntervalMs) return false;
  lastReportMs = now;
  return true;
}
}

bool SpiBusLock::begin() {
  if (_mutex) return true;
  _mutex = xSemaphoreCreateMutex();
  return _mutex != nullptr;
}

bool SpiBusLock::lock(TickType_t timeout) {
  if (!begin()) return false;
  const int64_t startedUs = esp_timer_get_time();
  if (xSemaphoreTake(_mutex, timeout) == pdTRUE) {
    lockedAtUs = esp_timer_get_time();
    return true;
  }
  // Zero-wait polls fail routinely; only report real waits.
  if (timeout >= pdMS_TO_TICKS(10) && reportAllowed(lastWaitReportMs)) {
    // A holder of "none" means the bus was free when the wait ended: the
    // waiting task did not get CPU time to take it.
    const int64_t heldUs = lockedAtUs ? esp_timer_get_time() - lockedAtUs : 0;
    Serial.printf("[SPI bus] %s (prio %u, core %d) waited %lld ms; holder=%s for %lld ms\n",
                  pcTaskGetName(nullptr), static_cast<unsigned>(uxTaskPriorityGet(nullptr)),
                  xPortGetCoreID(), (esp_timer_get_time() - startedUs) / 1000, holderName(),
                  xSemaphoreGetMutexHolder(_mutex) ? heldUs / 1000 : 0LL);
  }
  return false;
}

void SpiBusLock::unlock() {
  if (!_mutex) return;
  const int64_t heldUs = esp_timer_get_time() - lockedAtUs;
  xSemaphoreGive(_mutex);
  if (heldUs > LongHoldUs && reportAllowed(lastHoldReportMs)) {
    Serial.printf("[SPI bus] %s (prio %u, core %d) held the bus %lld ms\n",
                  pcTaskGetName(nullptr), static_cast<unsigned>(uxTaskPriorityGet(nullptr)),
                  xPortGetCoreID(), heldUs / 1000);
  }
}

const char *SpiBusLock::holderName() {
  if (!_mutex) return "none";
  TaskHandle_t holder = xSemaphoreGetMutexHolder(_mutex);
  return holder ? pcTaskGetName(holder) : "none";
}
