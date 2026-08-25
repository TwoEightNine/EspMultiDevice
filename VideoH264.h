#pragma once

#include <Arduino.h>

// Project-owned adapter around the pinned lib264 decoder. lib264 has
// architecture-specific front ends, so keeping its API behind this small
// boundary lets the player consume ordinary planar I420 output.
class VideoH264 {
public:
  struct Frame {
    const uint8_t *y;
    const uint8_t *u;
    const uint8_t *v;
    uint16_t width;
    uint16_t height;
    uint16_t yStride;
    uint16_t uStride;
    uint16_t vStride;
  };

  typedef bool (*FrameCallback)(const Frame &frame, void *context);

  VideoH264();
  ~VideoH264();

  bool begin(uint16_t maximumWidth, uint16_t maximumHeight);
  void end();
  bool decode(const uint8_t *annexB, uint32_t bytes, uint32_t timestamp,
              FrameCallback callback, void *context);
  bool flush(FrameCallback callback, void *context);
  const String &lastError() const;

private:
  void *_state;
  String _error;

  bool emitOutput(const void *operation, FrameCallback callback, void *context);
  bool setError(uint32_t errorCode, const __FlashStringHelper *fallback);
};
