# ESP Media

ESP Media is a touchscreen media device firmware for an ESP32-S3 N16R8. It runs on a 320 × 480 portrait, 3.95-inch ILI9488 display with an XPT2046 touch panel, SD-card storage, Wi-Fi, an INMP441 microphone, and a MAX98357A I2S amplifier.

## Features

- SD-card browser with folders, pagination, rename/delete actions, thumbnails, and optional PIN-offset protected storage.
- Manual USB Drive mode exposes the complete SD card as writable USB mass storage for macOS file transfer, with isolated transfer boot and safe-eject restart.
- Image viewer for BMP, PNG, baseline JPEG, GIF, and WebP, including animated GIF/WebP playback backed by SD caches and PSRAM.
- WAV/MP3 playback, 16 kHz mono WAV recording, configurable persisted volume, and built-in RGB LED settings.
- Direct MP4/3GP video streaming for H.264, H.263, and MPEG-4 Part 2 at up to 320 × 480 and 30fps, with AAC-LC, MP3, or AMR-NB audio where compatible. A second-core deadline scheduler decouples video decoding from non-blocking buffered audio output and does not create an SD cache.
- Wi-Fi management, UTC+4 SNTP time, a lightweight HTTP/HTTPS browser, and an interval-workout timer with audio cues.
- A screen-scoped, single-turn Xiaozhi voice assistant with animated state, STT/TTS captions, verified HTTPS/WSS, and half-duplex Opus audio.
- Runtime SD-card removal/reinsertion handling, touch mesh calibration, battery reporting, and a shared non-blocking screen/navigation model.

## Hardware profile

The supported profile is **ESP32S3 Dev Module**, 16 MiB flash, 8 MiB OPI PSRAM, and a 240 MHz CPU. Select the project **Custom** partition table: it provides two 6.25 MiB OTA application slots and 3.375 MiB SPIFFS. The default 1.2 MiB application partition is too small.

The default pin map and important electrical constraints are in [docs/hardware.md](docs/hardware.md). In particular, configure TFT_eSPI at the library level before compiling.

## Setup and build

1. Install Arduino-ESP32 3.3.8 and select **ESP32S3 Dev Module**, **16MB Flash**, **OPI PSRAM**, **240MHz**, and **Custom** partition scheme. Select **USB-OTG (TinyUSB)**, disable **USB CDC On Boot**, **USB Firmware MSC On Boot**, and **USB DFU On Boot**, then select **UART0 / Hardware CDC** upload mode.
2. Install these Arduino libraries through Library Manager:
   - `TFT_eSPI`
   - `PNGdec`
   - `JPEGDEC`
3. Configure the `TFT_eSPI` library’s `User_Setup.h` with the ILI9488 8-bit parallel pin block in [docs/hardware.md](docs/hardware.md#tft_espi-configuration).
4. Run `tools/build_esp32s3.sh`, or open `esp_media.ino` with the exact profile above and build/upload. The script additionally enforces the real 6,553,600-byte application limit, which Arduino's generic Custom profile does not. `build_opt.h` is read automatically.
5. Format the SD card as FAT32 with an MBR partition map. See [docs/hardware.md](docs/hardware.md#sd-card-formatting).
6. For the assistant, copy src/services/assistant/LocalAssistantBackendConfig.example.h to the ignored LocalAssistantBackendConfig.h, set the self-hosted HTTPS endpoints, and paste the Caddy root CA. Copy the five 320×240 avatar PNGs (`w.png`, `n.png`, `s.png`, `h.png`, `t.png`) into `/.system/ai_avatars/default/` and the alpha-capable underlay `blur.png` into `/.system/ai_avatars/` on the SD card. The local backend matches the speaker's language, with Simplified Chinese rendered and spoken through the assistant-specific font and a supported Mandarin Edge voice. Non-Chinese replies can have a Mandarin accent because Edge TTS uses one fixed voice. Configure and validate the Mac backend through tools/xiaozhi_local/README.md. There is no public-service fallback.

Firmware upload and 115200-baud Serial output stay on the board's USB-to-UART connector. If its automatic reset cannot enter the downloader, hold **BOOT**, tap **RESET/EN**, release **RESET/EN**, then release **BOOT** and upload again. USB Drive uses the second connector wired directly to the ESP32-S3 native USB D-/D+ pins (GPIO 19/20); it remains inactive during normal boot. Start **Settings → USB Drive**, connect the native port, transfer files in Finder, and eject the volume before unplugging it.

The first upload after changing from the old 3 MB/9.9 MB FATFS scheme must flash the new partition table. Preferences/NVS remains at the same offset and is compatible. Contents of the old, now-unused internal FATFS region are not preserved; ESP Media user media remains on SD.

The project vendors its remaining decoders and Stage 0 assistant dependencies under `third_party/`; they compile through controlled wrappers in `src/vendor_shims/`. Do not add those upstream source trees directly to the Arduino library search path.

## Project layout

```text
esp_media.ino      hardware composition, screen registration, main loop
build_opt.h        Arduino compiler options for bundled decoders
partitions.csv     16 MiB dual-OTA application/SPIFFS layout
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
