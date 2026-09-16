#pragma once

#include <Arduino.h>

#include "OpusCodec.h"

namespace AssistantAudio {

class OpusDecoder {
public:
  static constexpr size_t MaxPacketBytes = OpusCodec::MaxPacketBytes;
  static constexpr size_t MaxFrameSamples = 1440;

  bool begin(uint32_t sampleRate);
  void end();
  bool isReady() const;
  uint32_t sampleRate() const;
  size_t frameSamples() const;
  int decode(const uint8_t *packet, size_t length, int16_t *pcm, size_t capacity);
  const char *lastError() const;

private:
  OpusCodec _codec;
};

}  // namespace AssistantAudio
