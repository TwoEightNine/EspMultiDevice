// Portable ESP32-S3 platform definitions for the generic lib264 C backend.
#ifndef _IH264_PLATFORM_MACROS_H_
#define _IH264_PLATFORM_MACROS_H_

#define CLIP_U8(x) CLIP3(0, 255, (x))
#define CLIP_S8(x) CLIP3(-128, 127, (x))
#define CLIP_U10(x) CLIP3(0, 1023, (x))
#define CLIP_S10(x) CLIP3(-512, 511, (x))
#define CLIP_U12(x) CLIP3(0, 4095, (x))
#define CLIP_S12(x) CLIP3(-2048, 2047, (x))
#define CLIP_U16(x) CLIP3(0, 65535, (x))
#define CLIP_S16(x) CLIP3(-32768, 32767, (x))
#define MEM_ALIGN8 __attribute__((aligned(8)))
#define MEM_ALIGN16 __attribute__((aligned(16)))
#define MEM_ALIGN32 __attribute__((aligned(32)))
#define SHL(x, y) (((y) < 32) ? ((x) << (y)) : 0)
#define SHR(x, y) (((y) < 32) ? ((x) >> (y)) : 0)
#define SHR_NEG(value, shift) ((shift) > 0 ? (value) >> (shift) : (value) << (-(shift)))
#define SHL_NEG(value, shift) ((shift) < 0 ? (value) >> (-(shift)) : (value) << (shift))
#define ITT_BIG_ENDIAN(x) (((x) << 24) | (((x) & 0x0000ff00) << 8) | \
                           (((x) & 0x00ff0000) >> 8) | ((UWORD32)(x) >> 24))
#define NOP(count) do { for (UWORD32 i = 0; i < (count); i++) {} } while (0)
#define PLD(address)
#define DATA_SYNC() __sync_synchronize()
#define PREFETCH(ptr, type)
#define INLINE inline

static INLINE UWORD32 CLZ(UWORD32 value) {
  return value ? (UWORD32)__builtin_clz(value) : 32;
}

static INLINE UWORD32 CTZ(UWORD32 value) {
  return value ? (UWORD32)__builtin_ctz(value) : 31;
}

#endif
