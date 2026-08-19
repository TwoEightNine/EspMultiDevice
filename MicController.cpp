#include "MicController.h"

#include <esp_idf_version.h>
#include <string.h>

namespace {
#if ESP_IDF_VERSION_MAJOR >= 5
static const i2s_comm_format_t I2S_COMM_FORMAT = I2S_COMM_FORMAT_STAND_I2S;
#else
static const i2s_comm_format_t I2S_COMM_FORMAT = I2S_COMM_FORMAT_I2S;
#endif

int16_t clampToPcm16(int32_t sample) {
  if (sample > 32767) {
    return 32767;
  }

  if (sample < -32768) {
    return -32768;
  }

  return (int16_t)sample;
}
}

MicController::MicController(int bclkPin,
                             int wsPin,
                             int dataPin,
                             int powerSensePin,
                             i2s_port_t port,
                             uint32_t sampleRate)
  : _bclkPin(bclkPin),
    _wsPin(wsPin),
    _dataPin(dataPin),
    _powerSensePin(powerSensePin),
    _port(port),
    _sampleRate(sampleRate),
    _ready(false),
    _driverInstalled(false),
    _recording(false),
    _lastError(F("Not initialized")) {
}

bool MicController::begin() {
  if (_powerSensePin != NoPowerSensePin) {
    pinMode(_powerSensePin, INPUT_PULLDOWN);
  }

  _ready = true;
  _driverInstalled = false;
  _recording = false;
  _lastError = F("OK");

  return _ready;
}

bool MicController::isReady() const {
  return _ready;
}

bool MicController::hasPowerSense() const {
  return _powerSensePin != NoPowerSensePin;
}

bool MicController::isPowered() const {
  if (_powerSensePin == NoPowerSensePin) {
    return true;
  }

  return digitalRead(_powerSensePin) == HIGH;
}

bool MicController::start() {
  if (!_ready) {
    setError(F("Mic I2S is not ready"));
    return false;
  }

  if (!isPowered()) {
    setError(F("Mic power switch is off"));
    return false;
  }

  if (!_driverInstalled && !configureI2s()) {
    return false;
  }

  _recording = true;
  _lastError = F("Recording");
  return true;
}

void MicController::stop() {
  _recording = false;

  if (_driverInstalled) {
    i2s_driver_uninstall(_port);
    _driverInstalled = false;
  }

  pinMode(_bclkPin, INPUT);
  pinMode(_wsPin, INPUT);
  pinMode(_dataPin, INPUT);
}

bool MicController::isRecording() const {
  return _recording;
}

uint32_t MicController::sampleRate() const {
  return _sampleRate;
}

const String &MicController::lastError() const {
  return _lastError;
}

size_t MicController::readPcm16(int16_t *samples, size_t maxSamples) {
  if (!_recording || !_driverInstalled || !samples || maxSamples == 0 || !isPowered()) {
    return 0;
  }

  int32_t raw[RawChunkSamples];
  size_t samplesToRead = maxSamples < RawChunkSamples ? maxSamples : RawChunkSamples;
  size_t bytesRead = 0;

  esp_err_t result = i2s_read(
    _port,
    raw,
    samplesToRead * sizeof(raw[0]),
    &bytesRead,
    10 / portTICK_PERIOD_MS
  );

  if (result != ESP_OK || bytesRead == 0) {
    return 0;
  }

  size_t readCount = bytesRead / sizeof(raw[0]);
  for (size_t i = 0; i < readCount; i++) {
    samples[i] = clampToPcm16(raw[i] >> 14);
  }

  return readCount;
}

bool MicController::configureI2s() {
  i2s_config_t i2sConfig;
  memset(&i2sConfig, 0, sizeof(i2sConfig));
  i2sConfig.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  i2sConfig.sample_rate = _sampleRate;
  i2sConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  i2sConfig.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  i2sConfig.communication_format = I2S_COMM_FORMAT;
  i2sConfig.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  i2sConfig.dma_buf_count = 4;
  i2sConfig.dma_buf_len = 256;
  i2sConfig.use_apll = false;

  esp_err_t result = i2s_driver_install(_port, &i2sConfig, 0, nullptr);
  if (result != ESP_OK) {
    setError(F("Mic I2S driver install failed"));
    return false;
  }

  i2s_pin_config_t pinConfig;
  memset(&pinConfig, 0, sizeof(pinConfig));
#if ESP_IDF_VERSION_MAJOR >= 5
  pinConfig.mck_io_num = I2S_PIN_NO_CHANGE;
#endif
  pinConfig.bck_io_num = _bclkPin;
  pinConfig.ws_io_num = _wsPin;
  pinConfig.data_out_num = I2S_PIN_NO_CHANGE;
  pinConfig.data_in_num = _dataPin;

  result = i2s_set_pin(_port, &pinConfig);
  if (result != ESP_OK) {
    i2s_driver_uninstall(_port);
    setError(F("Mic I2S pin setup failed"));
    return false;
  }

  i2s_zero_dma_buffer(_port);
  _driverInstalled = true;
  return true;
}

void MicController::setError(const __FlashStringHelper *message) {
  _lastError = message;
}
