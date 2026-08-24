#include "SpeakerController.h"

#include <esp_idf_version.h>
#include <string.h>

namespace {
#if ESP_IDF_VERSION_MAJOR >= 5
static const i2s_comm_format_t I2S_COMM_FORMAT = I2S_COMM_FORMAT_STAND_I2S;
#else
static const i2s_comm_format_t I2S_COMM_FORMAT = I2S_COMM_FORMAT_I2S;
#endif
}

SpeakerController::SpeakerController(int bclkPin,
                                     int wsPin,
                                     int dataPin,
                                     int enablePin,
                                     i2s_port_t port,
                                     uint32_t sampleRate)
  : _bclkPin(bclkPin),
    _wsPin(wsPin),
    _dataPin(dataPin),
    _enablePin(enablePin),
    _port(port),
    _sampleRate(sampleRate),
    _ready(false),
    _controlStarted(false),
    _lastError(F("Not initialized")) {
}

void SpeakerController::beginControl() {
  if (_controlStarted) {
    return;
  }

  if (_enablePin != NoEnablePin) {
    pinMode(_enablePin, OUTPUT);
    digitalWrite(_enablePin, LOW);
  }

  _controlStarted = true;
}

bool SpeakerController::begin() {
  if (_ready) {
    return true;
  }

  mute();
  _ready = configureI2s();
  if (_ready) {
    _lastError = F("OK");
  }

  return _ready;
}

bool SpeakerController::isReady() const {
  return _ready;
}

void SpeakerController::mute() {
  beginControl();

  if (_enablePin != NoEnablePin) {
    digitalWrite(_enablePin, LOW);
  }
}

void SpeakerController::unmute() {
  beginControl();

  if (_enablePin != NoEnablePin) {
    digitalWrite(_enablePin, HIGH);
  }
}

bool SpeakerController::setSampleRate(uint32_t sampleRate) {
  if (!_ready) {
    setError(F("Speaker I2S is not ready"));
    return false;
  }

  esp_err_t result = i2s_set_clk(_port, sampleRate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
  if (result != ESP_OK) {
    setError(F("Speaker sample-rate change failed"));
    return false;
  }

  _sampleRate = sampleRate;
  return true;
}

uint32_t SpeakerController::sampleRate() const {
  return _sampleRate;
}

const String &SpeakerController::lastError() const {
  return _lastError;
}

size_t SpeakerController::writePcm16(const int16_t *samples, size_t sampleCount, uint32_t timeoutMs) {
  if (!_ready || !samples || sampleCount == 0) {
    return 0;
  }

  size_t bytesWritten = 0;
  esp_err_t result = i2s_write(
    _port,
    samples,
    sampleCount * sizeof(samples[0]),
    &bytesWritten,
    timeoutMs / portTICK_PERIOD_MS
  );

  if (result != ESP_OK) {
    setError(F("Speaker I2S write failed"));
    return 0;
  }

  return bytesWritten / sizeof(samples[0]);
}

void SpeakerController::stop() {
  if (!_ready) {
    return;
  }

  mute();
  i2s_driver_uninstall(_port);
  _ready = false;
  pinMode(_bclkPin, INPUT);
  pinMode(_wsPin, INPUT);
  pinMode(_dataPin, INPUT);
}

bool SpeakerController::configureI2s() {
  i2s_config_t i2sConfig;
  memset(&i2sConfig, 0, sizeof(i2sConfig));
  i2sConfig.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  i2sConfig.sample_rate = _sampleRate;
  i2sConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  i2sConfig.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  i2sConfig.communication_format = I2S_COMM_FORMAT;
  i2sConfig.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  // Keep enough queued audio to cover a short burst of display traffic. The
  // animated image viewer updates the TFT in bounded bands, but the larger
  // queue also protects playback from SD/cache and filesystem jitter.
  i2sConfig.dma_buf_count = 8;
  i2sConfig.dma_buf_len = 512;
  i2sConfig.use_apll = false;
  i2sConfig.tx_desc_auto_clear = true;

  esp_err_t result = i2s_driver_install(_port, &i2sConfig, 0, nullptr);
  if (result != ESP_OK) {
    setError(F("Speaker I2S driver install failed"));
    return false;
  }

  i2s_pin_config_t pinConfig;
  memset(&pinConfig, 0, sizeof(pinConfig));
#if ESP_IDF_VERSION_MAJOR >= 5
  pinConfig.mck_io_num = I2S_PIN_NO_CHANGE;
#endif
  pinConfig.bck_io_num = _bclkPin;
  pinConfig.ws_io_num = _wsPin;
  pinConfig.data_out_num = _dataPin;
  pinConfig.data_in_num = I2S_PIN_NO_CHANGE;

  result = i2s_set_pin(_port, &pinConfig);
  if (result != ESP_OK) {
    i2s_driver_uninstall(_port);
    setError(F("Speaker I2S pin setup failed"));
    return false;
  }

  i2s_zero_dma_buffer(_port);
  return true;
}

void SpeakerController::setError(const __FlashStringHelper *message) {
  _lastError = message;
}
