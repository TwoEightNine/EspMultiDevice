#pragma once

#include <Arduino.h>
#include <driver/i2s.h>

class SpeakerController {
public:
  static const int NoEnablePin = -1;

  SpeakerController(int bclkPin,
                    int wsPin,
                    int dataPin,
                    int enablePin = NoEnablePin,
                    i2s_port_t port = I2S_NUM_1,
                    uint32_t sampleRate = 16000);

  void beginControl();
  bool begin();
  // A zero sample rate retains the currently configured I2S rate.
  bool start(uint32_t sampleRate);
  bool isReady() const;
  void mute();
  void unmute();
  bool setSampleRate(uint32_t sampleRate);
  uint32_t sampleRate() const;
  void setVolume(uint8_t level);
  uint8_t volume() const;
  const String &lastError() const;

  size_t writePcm16(const int16_t *samples, size_t sampleCount, uint32_t timeoutMs = 100);
  bool drain(uint32_t timeoutMs = 2000);
  void stop();

private:
  int _bclkPin;
  int _wsPin;
  int _dataPin;
  int _enablePin;
  i2s_port_t _port;
  uint32_t _sampleRate;
  uint8_t _volume;
  bool _ready;
  bool _controlStarted;
  String _lastError;

  static const size_t VolumeBufferSamples = 256;
  static const uint32_t StartupSilenceMs = 10;
  int16_t _volumeBuffer[VolumeBufferSamples];

  bool configureI2s();
  void setError(const __FlashStringHelper *message);
};
