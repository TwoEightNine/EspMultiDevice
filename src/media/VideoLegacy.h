#pragma once

#include <Arduino.h>

#include "VideoH264.h"

// Adapter around the minimal, generic-C FFmpeg build used for legacy 3GPP
// H.263 and MPEG-4 Part 2 samples. Its frame contract intentionally matches
// VideoH264 so the cache builder stays independent of the source codec.
class VideoLegacy {
public:
  enum class Codec : uint8_t { H263, Mpeg4Part2 };

  typedef VideoH264::Frame Frame;
  typedef VideoH264::FrameCallback FrameCallback;

  VideoLegacy();
  ~VideoLegacy();

  bool begin(Codec codec, uint16_t maximumWidth, uint16_t maximumHeight,
             const uint8_t *configuration, uint32_t configurationBytes);
  void end();
  bool decode(const uint8_t *sample, uint32_t bytes, uint32_t timestamp,
              FrameCallback callback, void *context);
  bool flush(FrameCallback callback, void *context);
  const String &lastError() const;

private:
  void *_context;
  void *_frame;
  void *_packet;
  uint16_t _maximumWidth;
  uint16_t _maximumHeight;
  String _error;

  bool receiveFrames(FrameCallback callback, void *context, bool draining);
  bool emitFrame(FrameCallback callback, void *context);
  bool setError(int code, const __FlashStringHelper *fallback);
};
