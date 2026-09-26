#pragma once

#include <limits.h>

// Arduino-ESP32's Xtensa C++ limits.h does not publish the C99 long-long
// limit macros unless its libc feature gates are enabled. Lua tests them when
// selecting its integer ABI. Keep the same 64-bit ABI in C and C++ units.
#ifndef LLONG_MAX
#define LLONG_MAX __LONG_LONG_MAX__
#endif
#ifndef LLONG_MIN
#define LLONG_MIN (-LLONG_MAX - 1LL)
#endif
#ifndef ULLONG_MAX
#define ULLONG_MAX (2ULL * LLONG_MAX + 1ULL)
#endif
