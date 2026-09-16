#pragma once

#include <Arduino.h>

struct OpusEncoder;
struct OpusDecoder;

class OpusCodec {
public:
  static constexpr uint32_t EncoderSampleRate = 16000;
  static constexpr size_t EncoderFrameSamples = 960;
  static constexpr size_t MaxPacketBytes = 1275;
  static constexpr uint32_t DefaultBitrate = 24000;
  static constexpr int DefaultComplexity = 5;

  OpusCodec();
  ~OpusCodec();

  OpusCodec(const OpusCodec &) = delete;
  OpusCodec &operator=(const OpusCodec &) = delete;

  bool begin(uint32_t decoderSampleRate);
  bool beginEncoder();
  bool beginDecoder(uint32_t decoderSampleRate);
  void end();
  bool isReady() const;
  bool encoderReady() const;
  bool decoderReady() const;
  uint32_t decoderSampleRate() const;
  size_t decoderFrameSamples() const;

  int encode60Ms(const int16_t *pcm,
                 size_t sampleCount,
                 uint8_t *packet,
                 size_t packetCapacity);
  int decode(const uint8_t *packet,
             size_t packetLength,
             int16_t *pcm,
             size_t pcmCapacity,
             bool decodeFec = false);

  int lastErrorCode() const;
  const char *lastError() const;

private:
  OpusEncoder *_encoder;
  OpusDecoder *_decoder;
  uint32_t _decoderSampleRate;
  size_t _decoderFrameSamples;
  int _lastError;

  bool allocateEncoder();
  bool allocateDecoder(uint32_t sampleRate);
  void releaseEncoder();
  void releaseDecoder();
};
