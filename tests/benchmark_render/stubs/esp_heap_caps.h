// Host stand-in for ESP-IDF capability allocation.
#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_8BIT 4
inline void *heap_caps_malloc(size_t size, int) { return malloc(size); }
inline void heap_caps_free(void *pointer) { free(pointer); }
