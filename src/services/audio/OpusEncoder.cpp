#include "OpusEncoder.h"

namespace AssistantAudio {

bool OpusEncoder::begin() { return _codec.beginEncoder(); }
void OpusEncoder::end() { _codec.end(); }
bool OpusEncoder::isReady() const { return _codec.encoderReady(); }
int OpusEncoder::encode(const int16_t *pcm, size_t samples, uint8_t *packet, size_t capacity) {
  return _codec.encode60Ms(pcm, samples, packet, capacity);
}
const char *OpusEncoder::lastError() const { return _codec.lastError(); }

}  // namespace AssistantAudio

