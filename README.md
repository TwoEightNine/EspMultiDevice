# ESP Media

ESP Media is a touchscreen media device firmware for an ESP32-S3 N16R8. It runs on a 320 × 480 portrait, 3.95-inch ILI9488 display with an XPT2046 touch panel, SD-card storage, Wi-Fi, an INMP441 microphone, and a MAX98357A I2S amplifier.

<!-- Add photos to docs/images/ and uncomment:
![ESP Media showing its Home screen](docs/images/device-front.jpg)
![Inside the device: board, display, and audio modules](docs/images/device-inside.jpg)
-->
_Photos of the assembled device are coming soon._

## Project status

ESP Media is a personal hobby project in active development, currently at firmware version 0.2.0. It is built and tested on a single hand-wired device, and the macOS-specific features (USB Drive and Mac Remote) are tested only with macOS. Until 1.0, the SD app SDK, on-card formats, and settings may change between minor versions; [CHANGELOG.md](CHANGELOG.md) records these changes. Bug reports, fixes, and ports to other boards are welcome; see [CONTRIBUTING.md](CONTRIBUTING.md).

## Features

- SD-card browser with folders, pagination, rename/delete actions, thumbnails, and optional PIN-offset protected storage.
- Read-only paged text viewing for `.txt`, `.json`, `.lua`, and `.config` files, including files in protected storage.
- Manual USB Drive mode exposes the complete SD card as writable USB mass storage for macOS file transfer, with isolated transfer boot and safe-eject restart.
- A Mac Remote screen exposes standard USB HID keyboard and media controls for YouTube, screenshots, screen lock, playback, mute, and volume without a companion Mac application or configured macOS Shortcut.
- Image viewer for BMP, PNG, baseline JPEG, GIF, and WebP, including animated GIF/WebP playback streamed from the source through PSRAM.
- WAV/MP3 playback, 16 kHz mono WAV recording, configurable persisted volume, and built-in RGB LED settings.
- Direct MP4/3GP video streaming for H.264, H.263, and MPEG-4 Part 2 at the source's full decodable size, downscaled to the portrait or on-demand landscape display at the best rate the device sustains, with AAC-LC, MP3, or AMR-NB audio where compatible. Complete local H.264/AAC fragmented MP4 files are also supported. A second-core scheduler decouples video decoding from non-blocking buffered audio output and does not create an SD cache.
- Wi-Fi management with a bounded Cloudflare internet speed test, a verified SD read/write speed test, UTC+4 SNTP time, a lightweight HTTP/HTTPS browser, and an interval-workout timer with audio cues.
- Optional, self-hosted: a stack-scoped Xiaozhi voice assistant with a persistent multi-turn WSS session that stays connected under screens it opens and closes only from its app-bar X, verified HTTPS/WSS, half-duplex Opus audio, authenticated device status/settings commands, normal-SD media search/open commands, file-location and paged directory-listing answers, and Browser search handoffs.
- Runtime SD-card removal/reinsertion handling, touch mesh calibration, battery reporting, an About screen with the firmware version, and a shared non-blocking screen/navigation model.
- SD-loaded Lua apps appear directly in Home's alphabetized, swipe-paged grid unless their manifest hides them or they are disabled in Settings → Apps, with versioned drawing, tone, microphone/WAV, app-file, permission-gated SD file management, host-rendered image viewing, permission-gated HTTP(S) requests with streaming JSON parsing, opening other apps by ID, and device-status APIs. Installing an app requires no firmware upload.

## Hardware

### Parts

| Part | Notes |
| --- | --- |
| ESP32-S3 N16R8 development board | 16 MiB flash and 8 MiB OPI PSRAM are required. It needs two USB connectors: one through a USB-to-UART bridge for upload and Serial, and one wired to the native USB pins for USB Drive and Mac Remote. The developer's board is an initial-revision ESP32-S3-DevKitC-1, whose RGB LED is on GPIO 48. |
| 3.95-inch 320 × 480 ILI9488 display module | 8-bit parallel interface with an XPT2046 resistive touch controller. |
| microSD card slot and card | SPI, sharing its bus with the touch controller; many display modules include one. Format the card as FAT32 with an MBR partition map. |
| INMP441 I2S MEMS microphone | Powered through a switch, whose output also goes to a sense GPIO. |
| MAX98357A I2S amplifier and a speaker | |
| Resistors | 10k pull-up on display CS; 100k pulldown on amplifier enable; 100k from amplifier GAIN to ground; two 100k and a 100nF capacitor for the battery divider. |
| Bulk capacitors | Low-ESR electrolytics plus 100nF ceramics at the amplifier, display, and ESP32 input. |
| Power | Regulated 5V rated for at least 3A peak. For battery power, use a protected cell with power-path charging and a 5V boost converter, or 2S with a buck converter. |

[docs/hardware.md](docs/hardware.md) gives the pin map, capacitor values, power design, and electrical constraints.

### Wiring

<!-- Add a wiring diagram as docs/images/wiring.png and uncomment:
![ESP Media wiring diagram](docs/images/wiring.png)
-->
_A wiring diagram is coming soon._ Until then, follow the [default wiring table](docs/hardware.md#default-wiring).

### Board profile

The supported profile is **ESP32S3 Dev Module**, 16 MiB flash, 8 MiB OPI PSRAM, and a 240 MHz CPU. Select the project **Custom** partition table: it provides two 6.25 MiB OTA application slots and 3.375 MiB SPIFFS. The default 1.2 MiB application partition is too small.

## Setup and build

1. Install Arduino-ESP32 3.3.8 and select **ESP32S3 Dev Module**, **16MB Flash**, **OPI PSRAM**, **240MHz**, and **Custom** partition scheme. Select **USB-OTG (TinyUSB)**, disable **USB CDC On Boot**, **USB Firmware MSC On Boot**, and **USB DFU On Boot**, then select **UART0 / Hardware CDC** upload mode.
2. Install these Arduino libraries through Library Manager:
   - `TFT_eSPI` 2.5.43
   - `PNGdec` 1.1.6
   - `JPEGDEC` 1.8.4
3. Leave TFT_eSPI's own `User_Setup.h` alone: the sketch's [`tft_setup.h`](tft_setup.h) supplies the ILI9488 8-bit parallel configuration, and TFT_eSPI uses it instead. Edit it only if your wiring differs from [docs/hardware.md](docs/hardware.md#tft_espi-configuration).
4. Run `tools/build_esp32s3.sh` (in Git Bash on Windows), or open `esp_media.ino` with the exact profile above and build/upload. The script additionally enforces the real 6,553,600-byte application limit, which Arduino's generic Custom profile does not. `tools/build_esp32s3.sh --profile` instead builds against the exact board package and library versions pinned in [`sketch.yaml`](sketch.yaml), which arduino-cli installs on first use.
5. Format the SD card as FAT32 with an MBR partition map. See [docs/hardware.md](docs/hardware.md#sd-card-formatting).
6. Optional: to use the voice Assistant, set up its self-hosted backend and configure the firmware as described in [docs/assistant-setup.md](docs/assistant-setup.md). Skip this step otherwise.

To try an SD app, copy `examples/apps/counter`, `examples/apps/workout`, `examples/apps/recorder`, `examples/apps/files`, `examples/apps/images`, `examples/apps/dvach` (a 2ch imageboard reader), or `examples/apps/launcher` together with `examples/apps/launcher_helper` to `/Apps/<name>` on the card. Its tile appears directly on Home. See [docs/external-apps.md](docs/external-apps.md) for the package and SDK contract.

The Mac Remote YouTube button opens Spotlight, types `https://www.youtube.com/`, selects the top result with Down Arrow, and submits it to the default browser. This requires the standard Command-Space Spotlight shortcut and a keyboard input source that types ASCII URL characters from US-keyboard positions. The remaining actions use standard HID media keys or macOS keyboard shortcuts and need no setup.

Firmware upload and 115200-baud Serial output stay on the board's USB-to-UART connector. If its automatic reset cannot enter the downloader, hold **BOOT**, tap **RESET/EN**, release **RESET/EN**, then release **BOOT** and upload again. Mac Remote and USB Drive use the second connector wired directly to the ESP32-S3 native USB D-/D+ pins (GPIO 19/20). It remains inactive until Mac Remote is opened or the device restarts into USB Drive. Mac Remote stays enumerated until reset after its first use. USB Drive remains an isolated MSC-only boot; eject its volume in Finder before unplugging it.

The first upload after changing from the old 3 MB/9.9 MB FATFS scheme must flash the new partition table. Preferences/NVS remains at the same offset and is compatible. Contents of the old, now-unused internal FATFS region are not preserved; ESP Media user media remains on SD.

The project vendors its remaining decoders and the Assistant's Opus codec under `third_party/`; they compile through controlled wrappers in `src/vendor_shims/`. Do not add those upstream source trees directly to the Arduino library search path.

## Project layout

```text
esp_media.ino      hardware composition, screen registration, main loop
build_opt.h        Arduino compiler defines for bundled decoders
sketch.yaml        arduino-cli profile pinning the board package and libraries
tft_setup.h        TFT_eSPI display driver and pin configuration
partitions.csv     16 MiB dual-OTA application/SPIFFS layout
src/app/           Navigator and screen stack
src/core/          display, touch, battery, SPI lock, base Screen contract
src/features/      user-visible screens grouped by capability
examples/apps/     sample SD app packages
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
- [Optional Assistant setup](docs/assistant-setup.md)
- [Assistant device-command contributor guide](docs/agentic-commands.md)
- [Hardware, wiring, power, and SD preparation](docs/hardware.md)
- [Development, dependencies, licenses, and fixtures](docs/development.md)
- [SD app package and SDK contract](docs/external-apps.md)

## Contributing

Contributions are welcome. [CONTRIBUTING.md](CONTRIBUTING.md) explains how to build, test, and submit a change, and [AGENTS.md](AGENTS.md) gives the mandatory implementation rules, which apply to human and AI contributors alike. Everyone taking part follows the [Code of Conduct](CODE_OF_CONDUCT.md).

## License

```
ESP Media - touchscreen media device firmware for ESP32-S3
Copyright (C) 2026  TwoEightNine

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

Additional permission under GNU GPL version 3 section 7

If you modify this Program, or any covered work, by linking or
combining it with the Helix AAC decoder (or a modified version of
that library), containing parts covered by the terms of the
RealNetworks Public Source License or the RealNetworks Community
Source License, the licensors of this Program grant you additional
permission to convey the resulting work. Corresponding Source for a
non-source form of such a combination shall include the source code
for the parts of the Helix AAC decoder used as well as that of the
covered work.
```

The full license text is in [LICENSE](LICENSE). Bundled code under `third_party/`, the generated fonts, and the linked Arduino libraries keep their own licenses; [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) lists them and the notice files that must accompany a source or binary distribution.

### Logo

The TwoEightNine logo shown on the splash and About screens is part of the author's brand and is not covered by the GPL. You may use and redistribute it as is, unmodified, including in builds and forks of ESP Media. You may not modify it, but a fork may remove or replace it. See [src/ui/LICENSE.logo](src/ui/LICENSE.logo).

### Patents

Some supported media formats, such as H.264 and AAC, may be covered by patents in some countries, and the bundled H.264 decoder's license explicitly grants no patent rights. This rarely matters for sharing source code. If you build and distribute devices running this firmware, check whether you need codec patent licenses.

## Security

See [SECURITY.md](SECURITY.md) for how to report a vulnerability and for the firmware's known security limits.
