#pragma once

#include <Arduino.h>

#include "OpusCodec.h"

namespace AssistantAudio {

class OpusEncoder {
public:
  static constexpr size_t FrameSamples = OpusCodec::EncoderFrameSamples;
  static constexpr size_t MaxPacketBytes = OpusCodec::MaxPacketBytes;

  bool begin();
  void end();
  bool isReady() const;
  int encode(const int16_t *pcm, size_t samples, uint8_t *packet, size_t capacity);
  const char *lastError() const;

private:
  OpusCodec _codec;
};

}  // namespace AssistantAudio

