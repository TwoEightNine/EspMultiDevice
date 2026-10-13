#pragma once

#include <stdint.h>
#include <string.h>

// Canonical 44-byte header for mono 16-bit PCM WAV files. The built-in
// recorder and Lua app recordings share this single encoder/validator.
namespace WavFormat {
constexpr size_t HeaderBytes = 44;

inline void putLe16(uint8_t *buffer, uint16_t value) {
  buffer[0] = value & 0xff;
  buffer[1] = (value >> 8) & 0xff;
}

inline void putLe32(uint8_t *buffer, uint32_t value) {
  buffer[0] = value & 0xff;
  buffer[1] = (value >> 8) & 0xff;
  buffer[2] = (value >> 16) & 0xff;
  buffer[3] = (value >> 24) & 0xff;
}

inline uint16_t readLe16(const uint8_t *buffer) {
  return (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8);
}

inline uint32_t readLe32(const uint8_t *buffer) {
  return (uint32_t)buffer[0]
    | ((uint32_t)buffer[1] << 8)
    | ((uint32_t)buffer[2] << 16)
    | ((uint32_t)buffer[3] << 24);
}

inline void makeHeader(uint8_t header[HeaderBytes], uint32_t sampleRate, uint32_t dataBytes) {
  memset(header, 0, HeaderBytes);
  memcpy(header + 0, "RIFF", 4);
  putLe32(header + 4, 36 + dataBytes);
  memcpy(header + 8, "WAVE", 4);
  memcpy(header + 12, "fmt ", 4);
  putLe32(header + 16, 16);
  putLe16(header + 20, 1);
  putLe16(header + 22, 1);
  putLe32(header + 24, sampleRate);
  putLe32(header + 28, sampleRate * 2);
  putLe16(header + 32, 2);
  putLe16(header + 34, 16);
  memcpy(header + 36, "data", 4);
  putLe32(header + 40, dataBytes);
}

// Accepts only the header layout makeHeader() writes, with a non-zero data
// length. The caller still bounds dataBytes by the real file size.
inline bool parseHeader(const uint8_t header[HeaderBytes], uint32_t &sampleRate,
                        uint32_t &dataBytes) {
  const bool valid = memcmp(header + 0, "RIFF", 4) == 0
    && memcmp(header + 8, "WAVE", 4) == 0
    && memcmp(header + 12, "fmt ", 4) == 0
    && memcmp(header + 36, "data", 4) == 0
    && readLe16(header + 20) == 1
    && readLe16(header + 22) == 1
    && readLe16(header + 34) == 16
    && readLe32(header + 40) > 0;
  if (!valid) return false;
  sampleRate = readLe32(header + 24);
  dataBytes = readLe32(header + 40);
  return true;
}
}
