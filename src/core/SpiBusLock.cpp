#include "SpiBusLock.h"

SemaphoreHandle_t SpiBusLock::_mutex = nullptr;

bool SpiBusLock::begin() {
  if (_mutex) return true;
  _mutex = xSemaphoreCreateMutex();
  return _mutex != nullptr;
}

bool SpiBusLock::lock(TickType_t timeout) {
  return begin() && xSemaphoreTake(_mutex, timeout) == pdTRUE;
}

void SpiBusLock::unlock() {
  if (_mutex) xSemaphoreGive(_mutex);
}
