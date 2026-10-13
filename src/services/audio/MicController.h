#pragma once

#include <Arduino.h>
#include <driver/i2s.h>

class MicController {
public:
  static const int NoPowerSensePin = -1;

  MicController(int bclkPin,
                int wsPin,
                int dataPin,
                int powerSensePin = NoPowerSensePin,
                i2s_port_t port = I2S_NUM_0,
                uint32_t sampleRate = 16000);

  bool begin();
  bool isReady() const;
  bool hasPowerSense() const;
  bool isPowered() const;
  bool start();
  void stop();
  bool isRecording() const;
  uint32_t sampleRate() const;
  const String &lastError() const;

  // A zero timeout returns only samples already captured by DMA.
  size_t readPcm16(int16_t *samples, size_t maxSamples, uint32_t timeoutMs = 10);

private:
  static const size_t RawChunkSamples = 128;

  int _bclkPin;
  int _wsPin;
  int _dataPin;
  int _powerSensePin;
  i2s_port_t _port;
  uint32_t _sampleRate;
  bool _ready;
  bool _driverInstalled;
  bool _recording;
  String _lastError;

  bool configureI2s();
  void setError(const __FlashStringHelper *message);
};
