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
  bool isReady() const;
  void mute();
  void unmute();
  bool setSampleRate(uint32_t sampleRate);
  uint32_t sampleRate() const;
  const String &lastError() const;

  size_t writePcm16(const int16_t *samples, size_t sampleCount, uint32_t timeoutMs = 100);
  void stop();

private:
  int _bclkPin;
  int _wsPin;
  int _dataPin;
  int _enablePin;
  i2s_port_t _port;
  uint32_t _sampleRate;
  bool _ready;
  String _lastError;

  bool configureI2s();
  void setError(const __FlashStringHelper *message);
};
