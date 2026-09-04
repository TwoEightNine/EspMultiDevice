# Hardware, wiring, power, and SD preparation

## Required board profile

Use an ESP32-S3 with 16 MiB flash, 8 MiB PSRAM, 240 MHz CPU, and an application partition of at least 1.5 MiB. **16M Flash (3MB APP/9.9MB FATFS)** is the validated N16R8 choice. GIF playback needs at least three 480×320 RGB565 PSRAM frames plus a 512 KiB reserve.

Keep normal peripherals within GPIO 0–21, 35–42, and 45–48. Avoid GPIO 0, 19/20, and 45/46 unless the exact board documents them as safe; they commonly affect bootstrapping or USB.

## Default wiring

| Purpose | ESP32-S3 GPIO | Notes |
| --- | --- | --- |
| ILI9488 data | 4–11 | 8-bit parallel `D0` through `D7` |
| ILI9488 control | WR 12, DC 13, RD 14, CS 15, RST 16 | Do not define `TFT_BL` for this module |
| SD SPI | MISO 39, SCK 40, MOSI 41, CS 42 | Shared with touch clock/data |
| XPT2046 touch | MISO 39, SCK 40, MOSI 41, CS 38 | `TP_IRQ` unconnected and polled |
| INMP441 | DOUT 17, BCLK 18, WS 21, power sense 48 | `L/R` to GND for left channel |
| MAX98357A | DIN 1, BCLK 18, LRC 21, enable 2 | 100k pulldown on enable recommended |
| Battery divider | ADC 3 | 100k/100k divider and 100nF midpoint capacitor |

The TFT is preferred on GPIO 4–16 because TFT_eSPI’s fast parallel path uses low GPIOs. Do **not** connect the TFT module `SDO`/`MISO` to the SD SPI bus: many ILI9488 modules fail to release it when deselected.

Set `SD_CARD_DETECT_PIN` in `esp_media.ino` only if a card-detect switch is wired. The common active-low detect switch matches the controller default; use `-1` without it.

## TFT_eSPI configuration

TFT_eSPI reads its library-level `User_Setup.h`, not a project-local copy. In `/Users/fkdplc/Documents/Arduino/libraries/TFT_eSPI/User_Setup.h`, enable only the ILI9488 driver and use this parallel block:

```cpp
#define TFT_PARALLEL_8_BIT
#define ILI9488_DRIVER
#define TFT_INVERSION_OFF

#define TFT_CS   15
#define TFT_DC   13
#define TFT_RST  16
#define TFT_WR   12
#define TFT_RD   14

#define TFT_D0    4
#define TFT_D1    5
#define TFT_D2    6
#define TFT_D3    7
#define TFT_D4    8
#define TFT_D5    9
#define TFT_D6   10
#define TFT_D7   11
```

Supply the display and SD module according to their module documentation. A pin marked `LED`, `LED_A`, or `A` is normally raw backlight supply, not a GPIO control.

## Audio connections

The INMP441 receives switched 3.3V through a physical mic-power switch; the same switched node connects to GPIO 48 for sensing. If no sense wire exists, set `MIC_POWER_SENSE_PIN` to `MicController::NoPowerSensePin`.

Use 5V for MAX98357A `VIN` when the module and power budget allow it, otherwise 3.3V. `GAIN` uses a 100k resistor to ground for 15dB gain. The speaker output is differential: connect both terminals only to the speaker, never to ground. Do not tie `SD`/`EN` to VIN because the firmware mutes it at boot/recording.

## Power design and fault testing

Use a regulated 5V rail rated for at least 3A peak at the device. Star-wire separate positive and ground pairs from the regulator to the ESP32, TFT/SD, and amplifier. Keep amplifier/display wiring short and thick; do not return their current through breadboard paths or a single ESP32 ground lead.

Place these capacitors at the loads:

- MAX98357A: 470–1000µF low-ESR (≥6.3V) plus 100nF ceramic.
- TFT: 220–470µF low-ESR (≥6.3V) plus 100nF ceramic.
- ESP32 input: 100–220µF (≥6.3V) plus 100nF ceramic.

A TP4056 is a charger/protection board, not a 5V converter or load-sharing power path. For a one-cell system, use protected battery/BMS, power-path charging, and a synchronous 5V boost rated for at least 3A peak; its battery peak can exceed 4.5A. A 2S battery plus BMS/charger and a 5V buck is often more robust.

Do not connect external 5V to a board’s `VIN` while USB is attached unless its schematic explicitly supports dual power. The safe development arrangement is USB for the ESP32, external 5V for TFT/amplifier, common ground, and no positive backfeed to USB or board VIN.

For reset diagnosis, capture complete boot logs at 115200 baud. Test the intended final supply through: ESP32 alone; display/SD; Wi-Fi scans; then continuous or bass-heavy sound at volumes 3, 5, and 10. Measure 5V at ESP32 input and amplifier pins under load. If disconnecting only amplifier VIN stops resets, fix the rail, wiring, and capacitance rather than firmware.

## SD card formatting

Format the card as FAT32 using an MBR partition map. In macOS Disk Utility, choose **View → Show All Devices**, select the physical card, then select MBR scheme. If needed, after confirming the device identifier from `diskutil list`:

```sh
diskutil eraseDisk FAT32 ESPMEDIA MBRFormat /dev/disk4
```

Replace `/dev/disk4` with the actual SD disk; the command erases that entire disk.
