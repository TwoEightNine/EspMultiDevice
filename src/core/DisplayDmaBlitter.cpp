#include "DisplayDmaBlitter.h"

#include <TFT_eSPI.h>
#include <driver/gpio.h>
#include <esp_lcd_io_i80.h>
#include <esp_lcd_panel_io.h>
#include <esp_rom_gpio.h>
#include <soc/gpio_sig_map.h>
#include <soc/lcd_periph.h>

#if defined(TFT_PARALLEL_8_BIT) && SOC_LCDCAM_I80_LCD_SUPPORTED && defined(TFT_WR) && defined(TFT_DC)
#define ESP_MEDIA_DMA_BLIT 1
#else
#define ESP_MEDIA_DMA_BLIT 0
#endif

#if ESP_MEDIA_DMA_BLIT
namespace {
// 160 MHz PLL / 8. The ILI9488 accepts this write cycle on the reference
// wiring; lower it if long data wires show corrupted rows.
constexpr uint32_t PixelClockHz = 20000000;
constexpr uint32_t PsramBurstBytes = 64;
constexpr uint32_t FinishTimeoutUs = 200000;
constexpr int ColumnAddressSet = 0x2a;
constexpr int PageAddressSet = 0x2b;
constexpr int MemoryWrite = 0x2c;
const int DataPins[8] = {TFT_D0, TFT_D1, TFT_D2, TFT_D3, TFT_D4, TFT_D5, TFT_D6, TFT_D7};
const int ControlPins[2] = {TFT_DC, TFT_WR};

bool IRAM_ATTR transferDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *context) {
  *static_cast<volatile bool *>(context) = false;
  return false;
}

void setChipSelect(bool selected) {
#if defined(TFT_CS) && TFT_CS >= 0
  gpio_set_level((gpio_num_t)TFT_CS, selected ? 0 : 1);
#endif
}

// Bus creation or deletion may leave the pins as inputs or peripheral
// outputs; TFT_eSPI needs plain driven GPIO outputs.
void restoreGpioOutputs() {
  for (int pin : DataPins) gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
  for (int pin : ControlPins) gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
}
}  // namespace
#endif

DisplayDmaBlitter::~DisplayDmaBlitter() {
  end();
}

bool DisplayDmaBlitter::start(int16_t x, int16_t y, uint16_t width, uint16_t height, const uint16_t *pixels,
                              uint32_t maxTransferBytes) {
#if ESP_MEDIA_DMA_BLIT
  if (_disabled || !pixels || !width || !height || x < 0 || y < 0) return false;
  const uint32_t bytes = (uint32_t)width * height * sizeof(uint16_t);
  if (bytes > maxTransferBytes || ((uintptr_t)pixels & (PsramBurstBytes - 1))) return false;
  finish();
  if (!ensureBus(maxTransferBytes)) return false;

  const uint16_t right = x + width - 1;
  const uint16_t bottom = y + height - 1;
  const uint8_t columns[4] = {(uint8_t)(x >> 8), (uint8_t)x, (uint8_t)(right >> 8), (uint8_t)right};
  const uint8_t rows[4] = {(uint8_t)(y >> 8), (uint8_t)y, (uint8_t)(bottom >> 8), (uint8_t)bottom};
  routeToLcd();
  setChipSelect(true);
  esp_err_t error = esp_lcd_panel_io_tx_param(_io, ColumnAddressSet, columns, sizeof(columns));
  if (error == ESP_OK) error = esp_lcd_panel_io_tx_param(_io, PageAddressSet, rows, sizeof(rows));
  if (error == ESP_OK) {
    _transferring = true;
    // The driver writes the PSRAM cache back before GDMA reads the buffer.
    error = esp_lcd_panel_io_tx_color(_io, MemoryWrite, pixels, bytes);
  }
  if (error != ESP_OK) {
    _transferring = false;
    finish();
    disable("transfer", error);
    return false;
  }
  return true;
#else
  (void)x; (void)y; (void)width; (void)height; (void)pixels; (void)maxTransferBytes;
  return false;
#endif
}

bool DisplayDmaBlitter::busy() const {
  return _transferring;
}

bool DisplayDmaBlitter::finish() {
  if (!_ownsPins) return false;
  const uint32_t startedUs = micros();
  while (_transferring) {
    if ((uint32_t)(micros() - startedUs) >= FinishTimeoutUs) {
      // Never leave TFT_eSPI without its pins; a stuck transfer only reads.
      _transferring = false;
      disable("timeout", ESP_ERR_TIMEOUT);
      break;
    }
    taskYIELD();
  }
#if ESP_MEDIA_DMA_BLIT
  setChipSelect(false);
#endif
  routeToGpio();
  return true;
}

void DisplayDmaBlitter::end() {
#if ESP_MEDIA_DMA_BLIT
  finish();
  if (!_io && !_bus) return;
  if (_io) esp_lcd_panel_io_del(_io);
  if (_bus) esp_lcd_del_i80_bus(_bus);
  _io = nullptr;
  _bus = nullptr;
  _maxTransferBytes = 0;
  restoreGpioOutputs();
  routeToGpio();
#endif
}

bool DisplayDmaBlitter::ensureBus(uint32_t maxTransferBytes) {
#if ESP_MEDIA_DMA_BLIT
  if (_io && maxTransferBytes <= _maxTransferBytes) return true;
  end();
  esp_lcd_i80_bus_config_t busConfig = {};
  busConfig.dc_gpio_num = TFT_DC;
  busConfig.wr_gpio_num = TFT_WR;
  busConfig.clk_src = LCD_CLK_SRC_DEFAULT;
  for (uint8_t index = 0; index < 8; ++index) busConfig.data_gpio_nums[index] = DataPins[index];
  busConfig.bus_width = 8;
  busConfig.max_transfer_bytes = maxTransferBytes;
  busConfig.dma_burst_size = PsramBurstBytes;
  // Bus creation routes the pins to LCD_CAM while CS is high; they go back
  // to TFT_eSPI until the first transfer.
  esp_err_t error = esp_lcd_new_i80_bus(&busConfig, &_bus);
  if (error != ESP_OK) {
    _bus = nullptr;
    restoreGpioOutputs();
    routeToGpio();
    disable("bus", error);
    return false;
  }
  esp_lcd_panel_io_i80_config_t ioConfig = {};
  ioConfig.cs_gpio_num = -1;
  ioConfig.pclk_hz = PixelClockHz;
  ioConfig.trans_queue_depth = 2;
  ioConfig.on_color_trans_done = transferDone;
  ioConfig.user_ctx = const_cast<bool *>(&_transferring);
  ioConfig.lcd_cmd_bits = 8;
  ioConfig.lcd_param_bits = 8;
  ioConfig.dc_levels.dc_idle_level = 0;
  ioConfig.dc_levels.dc_cmd_level = 0;
  ioConfig.dc_levels.dc_dummy_level = 0;
  ioConfig.dc_levels.dc_data_level = 1;
  // Frame buffers hold little-endian RGB565; the panel expects the high byte
  // first, matching TFT_eSPI's setSwapBytes(true) output.
  ioConfig.flags.swap_color_bytes = 1;
  error = esp_lcd_new_panel_io_i80(_bus, &ioConfig, &_io);
  if (error != ESP_OK) {
    _io = nullptr;
    end();
    disable("io", error);
    return false;
  }
  _maxTransferBytes = maxTransferBytes;
  routeToGpio();
  Serial.printf("[Display] DMA blit ready pclk=%lu maxBytes=%lu\n", static_cast<unsigned long>(PixelClockHz),
                static_cast<unsigned long>(maxTransferBytes));
  return true;
#else
  (void)maxTransferBytes;
  return false;
#endif
}

void DisplayDmaBlitter::routeToLcd() {
#if ESP_MEDIA_DMA_BLIT
  const auto &signals = lcd_periph_i80_signals.buses[0];
  for (uint8_t index = 0; index < 8; ++index) {
    esp_rom_gpio_connect_out_signal(DataPins[index], signals.data_sigs[index], false, false);
  }
  esp_rom_gpio_connect_out_signal(TFT_DC, signals.dc_sig, false, false);
  esp_rom_gpio_connect_out_signal(TFT_WR, signals.wr_sig, false, false);
  _ownsPins = true;
#endif
}

void DisplayDmaBlitter::routeToGpio() {
#if ESP_MEDIA_DMA_BLIT
  for (int pin : DataPins) esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
  for (int pin : ControlPins) esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
#endif
  _ownsPins = false;
}

void DisplayDmaBlitter::disable(const char *reason, int error) {
  if (_disabled) return;
  _disabled = true;
  Serial.printf("[Display] DMA blit disabled (%s, %s); using TFT_eSPI\n", reason, esp_err_to_name(error));
}
