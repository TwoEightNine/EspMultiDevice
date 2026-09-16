#include "TlsMemoryScope.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>

#include <limits.h>

// Arduino-ESP32 3.3.8 declares these ESP-IDF C symbols with C++ linkage.
// Bind stable local names to the runtime symbols without exporting them from
// the application.
extern void *defaultTlsCalloc(size_t count, size_t size) asm("esp_mbedtls_mem_calloc");
extern void defaultTlsFree(void *pointer) asm("esp_mbedtls_mem_free");

namespace {
portMUX_TYPE allocatorMux = portMUX_INITIALIZER_UNLOCKED;
uint16_t allocatorUsers = 0;

void *psramTlsCalloc(size_t count, size_t size) {
  if (count && size > SIZE_MAX / count) return nullptr;
  if (psramFound()) {
    void *memory = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory) return memory;
  }
  return defaultTlsCalloc(count, size);
}
}  // namespace

TlsMemoryScope::TlsMemoryScope(bool enabled) : _active(false) {
  if (!enabled || !psramFound()) return;

  portENTER_CRITICAL(&allocatorMux);
  if (allocatorUsers > 0 ||
      mbedtls_platform_set_calloc_free(psramTlsCalloc, defaultTlsFree) == 0) {
    allocatorUsers++;
    _active = true;
  }
  portEXIT_CRITICAL(&allocatorMux);
}

TlsMemoryScope::~TlsMemoryScope() {
  if (!_active) return;

  portENTER_CRITICAL(&allocatorMux);
  if (allocatorUsers > 0) allocatorUsers--;
  if (allocatorUsers == 0) {
    mbedtls_platform_set_calloc_free(defaultTlsCalloc, defaultTlsFree);
  }
  portEXIT_CRITICAL(&allocatorMux);
}
