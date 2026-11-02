#pragma once

// TFT_eSPI configuration for ESP Media. TFT_eSPI 2.5 includes a tft_setup.h
// found on the include path instead of its library-level User_Setup.h, and the
// ESP32 core puts the sketch folder on that path for library builds too, so
// the global library configuration is never edited. Keep these pins in sync
// with docs/hardware.md.

#define USER_SETUP_INFO "ESP Media"

#define ILI9488_DRIVER
#define TFT_PARALLEL_8_BIT
#define TFT_INVERSION_OFF

#define TFT_CS    3  // External 10k pull-up keeps it high during reset
#define TFT_DC   13
#define TFT_RST  14
#define TFT_WR   12
#define TFT_RD   18

#define TFT_D0    4
#define TFT_D1    5
#define TFT_D2    6
#define TFT_D3    7
#define TFT_D4    8
#define TFT_D5    9
#define TFT_D6   10
#define TFT_D7   11

// The XPT2046 touch panel is driven by TouchController, not TFT_eSPI, so
// TOUCH_CS stays undefined.

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

#define SPI_FREQUENCY        40000000
#define SPI_READ_FREQUENCY   20000000
#define SPI_TOUCH_FREQUENCY   2500000
