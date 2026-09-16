#include "AssistantTlsAllocator.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>

#include <limits.h>

// Arduino-ESP32 3.3.8 declares these ESP-IDF C symbols with C++ linkage.
// Bind stable local names to the runtime symbols without exporting them from
// the application.
extern void *assistantDefaultTlsCalloc(size_t count, size_t size) asm("esp_mbedtls_mem_calloc");
extern void assistantDefaultTlsFree(void *pointer) asm("esp_mbedtls_mem_free");

namespace {
void *assistantTlsCalloc(size_t count, size_t size) {
  if (count && size > SIZE_MAX / count) return nullptr;
  if (psramFound()) {
    void *memory = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory) return memory;
  }
  return assistantDefaultTlsCalloc(count, size);
}
}  // namespace

AssistantTlsAllocatorScope::AssistantTlsAllocatorScope()
    : _active(psramFound() &&
              mbedtls_platform_set_calloc_free(assistantTlsCalloc, assistantDefaultTlsFree) == 0) {}

AssistantTlsAllocatorScope::~AssistantTlsAllocatorScope() {
  if (_active) {
    mbedtls_platform_set_calloc_free(assistantDefaultTlsCalloc, assistantDefaultTlsFree);
  }
}
