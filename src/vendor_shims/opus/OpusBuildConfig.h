#pragma once

#define HAVE_ALLOCA_H 1
#define HAVE_LRINT 1
#define HAVE_LRINTF 1
#define FIXED_POINT 1
#define DISABLE_FLOAT_API 1
#define HAVE_MEMORY_H 1
#define USE_ALLOCA 1
#define OPUS_BUILD 1
#define OPUS_XTENSA_LX7 1

#if defined(__GNUC__)
#pragma GCC optimize("O2")
#endif
