#include "SpeakerController.h"

#include <esp_idf_version.h>
#include <string.h>

namespace {
#if ESP_IDF_VERSION_MAJOR >= 5
static const i2s_comm_format_t I2S_COMM_FORMAT = I2S_COMM_FORMAT_STAND_I2S;
#else
static const i2s_comm_format_t I2S_COMM_FORMAT = I2S_COMM_FORMAT_I2S;
#endif

constexpr uint16_t I2sDmaDescriptors = 16;
constexpr uint16_t I2sMinFramesPerDescriptor = 64;
constexpr uint16_t I2sMaxFramesPerDescriptor = 512;
constexpr uint16_t I2sTargetQueueMs = 200;

uint16_t dmaFramesForRate(uint32_t sampleRate) {
  const uint32_t numerator = sampleRate * I2sTargetQueueMs;
  const uint32_t denominator = (uint32_t)I2sDmaDescriptors * 1000UL;
  uint32_t frames = (numerator + denominator - 1) / denominator;
  if (frames < I2sMinFramesPerDescriptor) frames = I2sMinFramesPerDescriptor;
  if (frames > I2sMaxFramesPerDescriptor) frames = I2sMaxFramesPerDescriptor;
  return (uint16_t)frames;
}
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
    _volume(10),
    _ready(false),
    _paused(false),
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

bool SpeakerController::start(uint32_t sampleRate) {
  // Keep the amplifier disabled until I2S is outputting a stable stream of
  // silence. This avoids exposing I2S startup transients at the speaker.
  mute();

  // Configure the DMA queue at the requested rate before installation. A
  // fixed 512-frame descriptor is about one second at 8 kHz, which makes
  // video time lead AMR audio even though both streams are otherwise aligned.
  if (sampleRate && !_ready) _sampleRate = sampleRate;
  if (!begin()) {
    return false;
  }
  if (sampleRate && !setSampleRate(sampleRate)) {
    stop();
    return false;
  }
  if (_paused && i2s_start(_port) != ESP_OK) {
    setError(F("Speaker I2S restart failed"));
    stop();
    return false;
  }

  i2s_zero_dma_buffer(_port);
  _paused = false;
  delay(StartupSilenceMs);
  unmute();
  return true;
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

void SpeakerController::setVolume(uint8_t level) {
  _volume = level > 10 ? 10 : level;
}

uint8_t SpeakerController::volume() const {
  return _volume;
}

const String &SpeakerController::lastError() const {
  return _lastError;
}

size_t SpeakerController::writePcm16(const int16_t *samples, size_t sampleCount, uint32_t timeoutMs) {
  if (!_ready || _paused || !samples || sampleCount == 0) {
    return 0;
  }

  if (_volume == 10) {
    size_t bytesWritten = 0;
    esp_err_t result = i2s_write(
      _port,
      samples,
      sampleCount * sizeof(samples[0]),
      &bytesWritten,
      timeoutMs / portTICK_PERIOD_MS
    );
    if (result != ESP_OK) {
      if (timeoutMs == 0 && result == ESP_ERR_TIMEOUT) return bytesWritten / sizeof(samples[0]);
      setError(F("Speaker I2S write failed"));
      return 0;
    }
    return bytesWritten / sizeof(samples[0]);
  }

  size_t writtenSamples = 0;
  while (writtenSamples < sampleCount) {
    size_t count = sampleCount - writtenSamples;
    if (count > VolumeBufferSamples) count = VolumeBufferSamples;

    for (size_t i = 0; i < count; i++) {
      _volumeBuffer[i] = (int16_t)(((int32_t)samples[writtenSamples + i] * _volume) / 10);
    }

    size_t bytesWritten = 0;
    esp_err_t result = i2s_write(
      _port,
      _volumeBuffer,
      count * sizeof(_volumeBuffer[0]),
      &bytesWritten,
      timeoutMs / portTICK_PERIOD_MS
    );
    size_t chunkSamples = bytesWritten / sizeof(_volumeBuffer[0]);
    writtenSamples += chunkSamples;
    if (result != ESP_OK) {
      if (timeoutMs == 0 && result == ESP_ERR_TIMEOUT) break;
      setError(F("Speaker I2S write failed"));
      break;
    }
    if (chunkSamples < count) break;
  }

  return writtenSamples;
}

size_t SpeakerController::tryWritePcm16(const int16_t *samples, size_t sampleCount) {
  if (sampleCount > VolumeBufferSamples) sampleCount = VolumeBufferSamples;
  return writePcm16(samples, sampleCount, 0);
}

bool SpeakerController::pauseOutput() {
  if (!_ready || _paused) return _ready;
  mute();
  if (i2s_stop(_port) != ESP_OK) {
    setError(F("Speaker I2S pause failed"));
    return false;
  }
  _paused = true;
  return true;
}

bool SpeakerController::resumeOutput() {
  if (!_ready || !_paused) return _ready;
  if (i2s_start(_port) != ESP_OK) {
    setError(F("Speaker I2S resume failed"));
    return false;
  }
  _paused = false;
  unmute();
  return true;
}

bool SpeakerController::drain(uint32_t timeoutMs) {
  if (!_ready) return true;
  // The legacy Arduino-ESP32 I2S API does not expose a TX-done primitive.
  // The rate-scaled DMA queue is about 200 ms, so allow that bounded queue to
  // finish before muting.
  const uint32_t drainMs = min<uint32_t>(timeoutMs, 260);
  delay(drainMs);
  return timeoutMs >= drainMs;
}

void SpeakerController::stop() {
  // Disable the amplifier before changing the I2S peripheral or pins.
  mute();

  if (!_ready) {
    return;
  }

  i2s_driver_uninstall(_port);
  _ready = false;
  _paused = false;
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
  // Hold roughly 200 ms at every supported rate. A 512-frame descriptor is
  // appropriate at 44.1 kHz but creates a full second of latency at 8 kHz.
  const uint16_t dmaFrames = dmaFramesForRate(_sampleRate);
#if ESP_IDF_VERSION_MAJOR >= 5
  i2sConfig.dma_desc_num = I2sDmaDescriptors;
  i2sConfig.dma_frame_num = dmaFrames;
#else
  i2sConfig.dma_buf_count = I2sDmaDescriptors;
  i2sConfig.dma_buf_len = dmaFrames;
#endif
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
