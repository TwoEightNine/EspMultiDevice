#pragma once

#include <Arduino.h>
#include <FS.h>

// The compact, display-ready cache shared by animated images and video. The
// payload contains a four-byte frame delay followed by packed RGB444 pixels.
// Version 4 is deliberately retained so previously created GIF/WebP caches
// remain valid and video can use the same well-tested storage contract.
namespace AnimationCache {
constexpr uint32_t Magic = 0x41474d45UL;  // "EMGA" little-endian
constexpr uint8_t Version = 4;
constexpr uint16_t HeaderBytes = 40;
constexpr uint16_t FrameHeaderBytes = 4;
constexpr uint16_t CanvasWidth = 480;
constexpr uint16_t CanvasHeight = 320;

inline uint16_t readLe16(const uint8_t *bytes) {
  return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

inline uint32_t readLe32(const uint8_t *bytes) {
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
         ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

inline void writeLe16(uint8_t *bytes, uint16_t value) {
  bytes[0] = (uint8_t)value;
  bytes[1] = (uint8_t)(value >> 8);
}

inline void writeLe32(uint8_t *bytes, uint32_t value) {
  bytes[0] = (uint8_t)value;
  bytes[1] = (uint8_t)(value >> 8);
  bytes[2] = (uint8_t)(value >> 16);
  bytes[3] = (uint8_t)(value >> 24);
}

inline bool writeHeader(fs::File &file, uint32_t sourceSize, uint32_t sourceWriteTime,
                        uint32_t sourceWidth, uint32_t sourceHeight, uint32_t frameCount,
                        int32_t loopCount, uint16_t drawWidth, uint16_t drawHeight) {
  uint8_t header[HeaderBytes] = {};
  writeLe32(header + 0, Magic);
  header[4] = Version;
  header[5] = 2;  // packed RGB444
  writeLe16(header + 8, CanvasWidth);
  writeLe16(header + 10, CanvasHeight);
  writeLe32(header + 12, sourceSize);
  writeLe32(header + 16, sourceWriteTime);
  writeLe32(header + 20, sourceWidth);
  writeLe32(header + 24, sourceHeight);
  writeLe32(header + 28, frameCount);
  writeLe32(header + 32, (uint32_t)loopCount);
  writeLe16(header + 36, drawWidth);
  writeLe16(header + 38, drawHeight);
  return file.write(header, sizeof(header)) == sizeof(header);
}

inline uint32_t rgb444Bytes(uint16_t drawWidth, uint16_t drawHeight) {
  return ((uint32_t)drawWidth * drawHeight * 3UL + 1UL) / 2UL;
}

inline uint32_t frameBytes(uint16_t drawWidth, uint16_t drawHeight) {
  return FrameHeaderBytes + rgb444Bytes(drawWidth, drawHeight);
}
}  // namespace AnimationCache
