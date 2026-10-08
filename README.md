# ESP Media

ESP Media is a touchscreen media device firmware for an ESP32-S3 N16R8. It runs on a 320 × 480 portrait, 3.95-inch ILI9488 display with an XPT2046 touch panel, SD-card storage, Wi-Fi, an INMP441 microphone, and a MAX98357A I2S amplifier.

## Features

- SD-card browser with folders, pagination, rename/delete actions, thumbnails, and optional PIN-offset protected storage.
- Read-only paged text viewing for `.txt`, `.json`, `.lua`, and `.config` files, including files in protected storage.
- Manual USB Drive mode exposes the complete SD card as writable USB mass storage for macOS file transfer, with isolated transfer boot and safe-eject restart.
- A Mac Remote screen exposes standard USB HID keyboard and media controls for YouTube, screenshots, screen lock, playback, mute, and volume without a companion Mac application or configured macOS Shortcut.
- Image viewer for BMP, PNG, baseline JPEG, GIF, and WebP, including animated GIF/WebP playback backed by SD caches and PSRAM.
- WAV/MP3 playback, 16 kHz mono WAV recording, configurable persisted volume, and built-in RGB LED settings.
- Direct MP4/3GP video streaming for H.264, H.263, and MPEG-4 Part 2 at the source's full decodable size, downscaled to the portrait or on-demand landscape display at the best rate the device sustains, with AAC-LC, MP3, or AMR-NB audio where compatible. Complete local H.264/AAC fragmented MP4 files are also supported. A second-core scheduler decouples video decoding from non-blocking buffered audio output and does not create an SD cache.
- Wi-Fi management with a bounded Cloudflare internet speed test, a verified SD read/write speed test, UTC+4 SNTP time, a lightweight HTTP/HTTPS browser, and an interval-workout timer with audio cues.
- A screen-scoped Xiaozhi voice assistant with a persistent multi-turn WSS session, verified HTTPS/WSS, half-duplex Opus audio, authenticated device status/settings commands, normal-SD media and file search/open commands, and Browser search handoffs.
- Runtime SD-card removal/reinsertion handling, touch mesh calibration, battery reporting, and a shared non-blocking screen/navigation model.
- SD-loaded Lua apps in a foreground Apps launcher, with versioned drawing, tone, app-file, and device-status APIs. Installing an app requires no firmware upload.

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
6. For the assistant, copy src/services/assistant/LocalAssistantBackendConfig.example.h to the ignored LocalAssistantBackendConfig.h, set the self-hosted HTTPS endpoints, and paste the Caddy root CA. Copy a non-interlaced 320×432 `background.png` to `/.system/ai_avatars/background.png`; it is drawn exactly below the app bar and contains the fixed chat-bubble artwork. Copy the five 320×240 avatar PNGs (`w.png`, `n.png`, `s.png`, `h.png`, `t.png`) into `/.system/ai_avatars/default/`. Optional additional sets use the same five filenames in `/.system/ai_avatars/<dir>/`; list them in `/.system/ai_avatars/avatars.config` as one `Name:dir` entry per line. A missing, corrupt, or wrong-sized background falls back to black. The Assistant shows only the latest live user/reply pair: user text is two lines with ellipsis, while assistant captions page through three spoken lines every two seconds. Its 60×60 avatar-overlay control is blue while ready, green while listening, and gray when unavailable. Hold it for 800 ms to enable screen-session auto-listen; a short cyan pulse confirms it, and the next tap disables it. The overflow menu contains New conversation and the Avatar submenu. Local captions disappear on screen close; the local backend retains a summary plus the last 12 completed exchanges per device until New conversation is confirmed. It uses OpenAI `gpt-transcribe`, GPT-5.6 Luna for all LLM work, and Azure neural voices for English, Simplified Chinese, and Russian. Configure the private OpenAI/Azure credentials and validate the custom pinned backend through tools/xiaozhi_local/README.md. There is no public-service fallback.

To try an SD app, copy `examples/apps/counter` to `/Apps/counter` or `examples/apps/workout` to `/Apps/workout` on the card and open **Apps** on Home. See [docs/external-apps.md](docs/external-apps.md) for the package and SDK contract.

The Mac Remote YouTube button opens Spotlight, types `https://www.youtube.com/`, selects the top result with Down Arrow, and submits it to the default browser. This requires the standard Command-Space Spotlight shortcut and a keyboard input source that types ASCII URL characters from US-keyboard positions. The remaining actions use standard HID media keys or macOS keyboard shortcuts and need no setup.

Firmware upload and 115200-baud Serial output stay on the board's USB-to-UART connector. If its automatic reset cannot enter the downloader, hold **BOOT**, tap **RESET/EN**, release **RESET/EN**, then release **BOOT** and upload again. Mac Remote and USB Drive use the second connector wired directly to the ESP32-S3 native USB D-/D+ pins (GPIO 19/20). It remains inactive until Mac Remote is opened or the device restarts into USB Drive. Mac Remote stays enumerated until reset after its first use. USB Drive remains an isolated MSC-only boot; eject its volume in Finder before unplugging it.

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
examples/apps/      sample SD app packages
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
- [Assistant device-command contributor guide](docs/agentic-commands.md)
- [Hardware, wiring, power, and SD preparation](docs/hardware.md)
- [Development, dependencies, licenses, and fixtures](docs/development.md)
- [SD app package and SDK contract](docs/external-apps.md)

## Contributing

Read [AGENTS.md](AGENTS.md) before changing the firmware. It gives the mandatory implementation rules and routes each type of change to the documentation that defines its contract.
