#include "OpusCodec.h"

#include <esp_heap_caps.h>
#include <stdlib.h>

OpusCodec::OpusCodec()
  : _encoder(nullptr),
    _decoder(nullptr),
    _decoderSampleRate(0),
    _decoderFrameSamples(0),
    _lastError(OPUS_OK) {
}

OpusCodec::~OpusCodec() {
  end();
}

bool OpusCodec::begin(uint32_t decoderSampleRate) {
  end();
  if (decoderSampleRate != 16000 && decoderSampleRate != 24000) {
    _lastError = OPUS_BAD_ARG;
    return false;
  }
  if (!allocateEncoder() || !allocateDecoder(decoderSampleRate)) {
    end();
    return false;
  }
  _lastError = OPUS_OK;
  return true;
}

void OpusCodec::end() {
  releaseDecoder();
  releaseEncoder();
  _decoderSampleRate = 0;
  _decoderFrameSamples = 0;
}

bool OpusCodec::isReady() const {
  return _encoder != nullptr && _decoder != nullptr;
}

uint32_t OpusCodec::decoderSampleRate() const {
  return _decoderSampleRate;
}

size_t OpusCodec::decoderFrameSamples() const {
  return _decoderFrameSamples;
}

int OpusCodec::encode60Ms(const int16_t *pcm,
                          size_t sampleCount,
                          uint8_t *packet,
                          size_t packetCapacity) {
  if (!_encoder || !pcm || sampleCount != EncoderFrameSamples || !packet ||
      packetCapacity < MaxPacketBytes) {
    _lastError = OPUS_BAD_ARG;
    return _lastError;
  }

  int result = opus_encode(_encoder,
                           pcm,
                           static_cast<int>(EncoderFrameSamples),
                           packet,
                           static_cast<opus_int32>(MaxPacketBytes));
  _lastError = result < 0 ? result : OPUS_OK;
  return result;
}

int OpusCodec::decode(const uint8_t *packet,
                      size_t packetLength,
                      int16_t *pcm,
                      size_t pcmCapacity,
                      bool decodeFec) {
  if (!_decoder || !packet || packetLength == 0 ||
      packetLength > MaxPacketBytes || !pcm ||
      pcmCapacity < _decoderFrameSamples) {
    _lastError = OPUS_BAD_ARG;
    return _lastError;
  }

  int result = opus_decode(_decoder,
                           packet,
                           static_cast<opus_int32>(packetLength),
                           pcm,
                           static_cast<int>(_decoderFrameSamples),
                           decodeFec ? 1 : 0);
  _lastError = result < 0 ? result : OPUS_OK;
  return result;
}

int OpusCodec::lastErrorCode() const {
  return _lastError;
}

const char *OpusCodec::lastError() const {
  return opus_strerror(_lastError);
}

bool OpusCodec::allocateEncoder() {
  int size = opus_encoder_get_size(1);
  if (size <= 0) {
    _lastError = OPUS_INTERNAL_ERROR;
    return false;
  }

  _encoder = static_cast<OpusEncoder *>(
    heap_caps_malloc(static_cast<size_t>(size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
  );
  if (!_encoder) {
    _lastError = OPUS_ALLOC_FAIL;
    return false;
  }

  _lastError = opus_encoder_init(_encoder,
                                 EncoderSampleRate,
                                 1,
                                 OPUS_APPLICATION_VOIP);
  if (_lastError != OPUS_OK) {
    releaseEncoder();
    return false;
  }

  _lastError = opus_encoder_ctl(_encoder, OPUS_SET_BITRATE(DefaultBitrate));
  if (_lastError == OPUS_OK) {
    _lastError = opus_encoder_ctl(_encoder, OPUS_SET_COMPLEXITY(DefaultComplexity));
  }
  if (_lastError != OPUS_OK) {
    releaseEncoder();
    return false;
  }
  return true;
}

bool OpusCodec::allocateDecoder(uint32_t sampleRate) {
  int size = opus_decoder_get_size(1);
  if (size <= 0) {
    _lastError = OPUS_INTERNAL_ERROR;
    return false;
  }

  _decoder = static_cast<OpusDecoder *>(
    heap_caps_malloc(static_cast<size_t>(size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
  );
  if (!_decoder) {
    _lastError = OPUS_ALLOC_FAIL;
    return false;
  }

  _lastError = opus_decoder_init(_decoder, sampleRate, 1);
  if (_lastError != OPUS_OK) {
    releaseDecoder();
    return false;
  }

  _decoderSampleRate = sampleRate;
  _decoderFrameSamples = (sampleRate * 60U) / 1000U;
  return true;
}

void OpusCodec::releaseEncoder() {
  if (_encoder) {
    free(_encoder);
    _encoder = nullptr;
  }
}

void OpusCodec::releaseDecoder() {
  if (_decoder) {
    free(_decoder);
    _decoder = nullptr;
  }
}
