#include "OpusDecoder.h"

namespace AssistantAudio {

bool OpusDecoder::begin(uint32_t sampleRate) { return _codec.beginDecoder(sampleRate); }
void OpusDecoder::end() { _codec.end(); }
bool OpusDecoder::isReady() const { return _codec.decoderReady(); }
uint32_t OpusDecoder::sampleRate() const { return _codec.decoderSampleRate(); }
size_t OpusDecoder::frameSamples() const { return _codec.decoderFrameSamples(); }
int OpusDecoder::decode(const uint8_t *packet, size_t length, int16_t *pcm, size_t capacity) {
  return _codec.decode(packet, length, pcm, capacity);
}
const char *OpusDecoder::lastError() const { return _codec.lastError(); }

}  // namespace AssistantAudio
