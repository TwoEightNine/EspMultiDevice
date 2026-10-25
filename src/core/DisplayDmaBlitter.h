#pragma once

#include <Arduino.h>

struct esp_lcd_i80_bus_t;
struct esp_lcd_panel_io_t;

// Sends RGB565 rectangles to the 8-bit parallel ILI9488 through the ESP32-S3
// LCD_CAM i80 peripheral and GDMA. TFT_eSPI bit-bangs the same wires through
// GPIO registers, so the data, DC, and WR pins are routed to LCD_CAM only for
// the duration of one transfer and returned to plain GPIO by finish(). CS stays
// a GPIO; it is high whenever the routing changes, so switch glitches are
// ignored by the panel.
//
// The blitter is UI-task only. DisplayController calls finish() before any
// TFT_eSPI access, so a caller that starts a transfer must keep its pixel
// buffer unchanged until busy() is false or finish() has returned.
class DisplayDmaBlitter {
public:
  ~DisplayDmaBlitter();

  // Native RGB565 pixels in PSRAM or DMA-capable RAM, 64-byte aligned.
  bool start(int16_t x, int16_t y, uint16_t width, uint16_t height, const uint16_t *pixels,
             uint32_t maxTransferBytes);
  bool busy() const;
  // Waits for an active transfer and returns the bus to TFT_eSPI. Returns true
  // when this call released the bus, so the caller must reset TFT_eSPI's
  // cached address window.
  bool finish();
  void end();

private:
  esp_lcd_i80_bus_t *_bus = nullptr;
  esp_lcd_panel_io_t *_io = nullptr;
  uint32_t _maxTransferBytes = 0;
  volatile bool _transferring = false;
  bool _ownsPins = false;
  bool _disabled = false;

  bool ensureBus(uint32_t maxTransferBytes);
  void routeToLcd();
  void routeToGpio();
  void disable(const char *reason, int error);
};
