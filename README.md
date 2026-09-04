# ESP Media

ESP Media is a touchscreen media device firmware for an ESP32-S3 N16R8. It runs on a 480 × 320, 3.95-inch ILI9488 display with an XPT2046 touch panel, SD-card storage, Wi-Fi, an INMP441 microphone, and a MAX98357A I2S amplifier.

## Features

- SD-card browser with folders, pagination, rename/delete actions, thumbnails, and optional PIN-offset protected storage.
- Image viewer for BMP, PNG, baseline JPEG, GIF, and WebP, including animated GIF/WebP playback backed by SD caches and PSRAM.
- WAV/MP3 playback, 16 kHz mono WAV recording, and configurable persisted volume.
- MP4/3GP video playback for H.264, H.263, and MPEG-4 Part 2 at up to 320 × 240, with AAC-LC, MP3, or AMR-NB audio where compatible.
- Wi-Fi management, UTC+4 SNTP time, a lightweight HTTP/HTTPS browser, and an interval-workout timer with audio cues.
- Runtime SD-card removal/reinsertion handling, touch mesh calibration, battery reporting, and a shared non-blocking screen/navigation model.

## Hardware profile

The supported profile is an ESP32-S3 with 16 MiB flash, 8 MiB PSRAM, and a 240 MHz CPU. Select an application partition of at least 1.5 MiB; **16M Flash (3MB APP/9.9MB FATFS)** is known to work. The default 1.2 MiB application partition is too small.

The default pin map and important electrical constraints are in [docs/hardware.md](docs/hardware.md). In particular, configure TFT_eSPI at the library level before compiling.

## Setup and build

1. Install the ESP32 board support in the Arduino IDE and select an ESP32-S3 N16R8-compatible board profile with PSRAM enabled.
2. Install these Arduino libraries through Library Manager:
   - `TFT_eSPI`
   - `PNGdec`
   - `JPEGDEC`
3. Configure the `TFT_eSPI` library’s `User_Setup.h` with the ILI9488 8-bit parallel pin block in [docs/hardware.md](docs/hardware.md#tft_espi-configuration).
4. Open `esp_media.ino` as the sketch, select the partition profile above, then build and upload. `build_opt.h` is automatically read by Arduino and supplies the project’s decoder include paths and PNG buffer setting.
5. Format the SD card as FAT32 with an MBR partition map. See [docs/hardware.md](docs/hardware.md#sd-card-formatting).

The project vendors the remaining decoders under `third_party/`; they compile through the controlled wrappers in `src/vendor_shims/`. Do not add those upstream source trees directly to the Arduino library search path.

## Project layout

```text
esp_media.ino      hardware composition, screen registration, main loop
build_opt.h        Arduino compiler options for bundled decoders
src/app/           Navigator and screen stack
src/core/          display, touch, battery, SPI lock, base Screen contract
src/features/      user-visible screens grouped by capability
src/media/         media-format registry, cache contract, video adapters
src/services/      audio, network, and storage controllers
src/ui/            shared UI kit, text, icons, logo, generated fonts
src/vendor_shims/  Arduino compilation wrappers for third_party/
third_party/       pinned upstream media decoders and licenses
tools/             reproducible asset and fixture generators
docs/              detailed developer and behavior documentation
```

## Documentation

- [Architecture and component contracts](docs/architecture.md)
- [UI Kit contract](docs/ui-kit.md)
- [Media support and SD cache formats](docs/media.md)
- [Browser behavior and security limits](docs/browser.md)
- [Storage, audio, network, and device behavior](docs/services.md)
- [Hardware, wiring, power, and SD preparation](docs/hardware.md)
- [Development, dependencies, licenses, and fixtures](docs/development.md)

## Contributing

Read [AGENTS.md](AGENTS.md) before changing the firmware. It gives the mandatory implementation rules and routes each type of change to the documentation that defines its contract.
