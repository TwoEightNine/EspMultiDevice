# Third-party notices

ESP Media is licensed under GPL-3.0-or-later with an additional permission for
the Helix AAC decoder; see [LICENSE](LICENSE) and the
[README license notice](README.md#license). The TwoEightNine logo is the
author's brand and is not GPL-licensed; see `src/ui/LICENSE.logo`. The
components below keep their
own licenses. Their full license texts and copyright notices are in the listed
files, which must accompany any source or binary distribution.

## Bundled source compiled into the firmware

| Component | Version | Use | License | Notice files |
| --- | --- | --- | --- | --- |
| [AnimatedGIF](https://github.com/bitbank2/AnimatedGIF) | 2.2.3 | GIF decoding | Apache-2.0 | `third_party/AnimatedGIF/LICENSE` |
| [libwebp](https://github.com/webmproject/libwebp) | 1.6.0, decoder/demux subset | WebP decoding | BSD-3-Clause | `third_party/libwebp/COPYING`, `PATENTS`, `AUTHORS` |
| [minimp3](https://github.com/lieff/minimp3) | header-only | MP3 decoding | CC0-1.0 | `third_party/minimp3/minimp3.h` |
| [minimp4](https://github.com/lieff/minimp4) | `5a212a18`, with project patches | MP4 demuxing | CC0-1.0 | `third_party/minimp4/LICENSE` |
| [lib264](https://github.com/ittiam-systems/lib264) | `ca1f8e0e`, generic C | H.264 decoding | BSD-3-Clause-Clear; grants no patent license | `third_party/lib264/LICENSE`, `LICENSE2` |
| [FFmpeg](https://ffmpeg.org/) | 9.0.1, H.263/MPEG-4 Part 2 subset | Legacy video decoding | LGPL-2.1-or-later | `third_party/ffmpeg/COPYING.LGPLv2.1`, `LICENSE.md` |
| [OpenCORE AMR](https://sourceforge.net/projects/opencore-amr/) | 0.1.6, AMR-NB decoder subset | AMR-NB decoding | Apache-2.0 | `third_party/opencore-amr/LICENSE` |
| Helix AAC decoder, via [arduino-libhelix](https://github.com/pschatzmann/arduino-libhelix) | `7d0444ca` | AAC decoding | RPSL-1.0 / RCSL (decoder core); GPL-3.0 (arduino-libhelix memory and log utilities) | `third_party/libhelix/RPSL.txt`, `RCSL.txt`, `LICENSE` |
| [Lua](https://www.lua.org/) | 5.4.9 | SD app interpreter | MIT | `third_party/lua/lua.h` (end of file) |
| [78/esp-opus](https://components.espressif.com/components/78/esp-opus) | 1.0.5, mono subset | Assistant Opus audio | BSD-3-Clause (Opus); component metadata says MIT | `third_party/esp-opus/COPYING`, `LICENSE_PLEASE_READ.txt`, `AUTHORS` |

Exact archives, commits, and checksums are recorded in each component's
`VERSION.txt` or `README.esp-media.md`, and in
`third_party/VIDEO_DEPENDENCIES.md`.

## Fonts compiled into the firmware

| Font | Generated header | License | Notice file |
| --- | --- | --- | --- |
| GNU FreeFont FreeSans 20120503 | `src/ui/fonts/BrowserFreeSans*.h` | GPL-3.0-or-later with the FreeFont font exception | `LICENSE.FreeFont` |
| Noto Sans CJK SC | `src/ui/fonts/AssistantNotoSansSC18.h` | OFL-1.1 | `src/ui/fonts/LICENSE.NotoSansCJKSC` |

## Icons compiled into the firmware

| Icons | Generated header | License | Notice file |
| --- | --- | --- | --- |
| [Material Symbols](https://github.com/google/material-design-icons) Outlined: zoom, fullscreen, media transport, volume, repeat, music note, microphone, and stop | `src/ui/icons.h` | Apache-2.0 | `src/ui/LICENSE.MaterialSymbols` |

## Build dependencies linked into the firmware

These are not stored in this repository. They are installed through the
Arduino Boards Manager or Library Manager, as described in the
[README](README.md#setup-and-build), and are linked into every firmware image. Anyone
distributing a firmware binary must include their notices too.

| Component | License |
| --- | --- |
| [Arduino-ESP32](https://github.com/espressif/arduino-esp32) 3.3.8, including ESP-IDF, mbedTLS, and TinyUSB | LGPL-2.1 (Arduino core); Apache-2.0 (ESP-IDF, mbedTLS); MIT (TinyUSB); see the core's own notices |
| [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) | FreeBSD (original code), BSD (Adafruit_GFX portions), and MIT (Adafruit_ILI9341 portions); see the library's `license.txt` |
| [PNGdec](https://github.com/bitbank2/PNGdec) | Apache-2.0 |
| [JPEGDEC](https://github.com/bitbank2/JPEGDEC) | Apache-2.0 |

## Tools that are not part of the firmware

The self-hosted Assistant backend overlay in `tools/xiaozhi_local/` patches
[xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server)
(MIT). Its notice is in
`tools/xiaozhi_local/server_overlay/XIAOZHI_MIT_LICENSE.txt`.

## Patents

Some of the media formats above may be covered by patents in some countries,
for example H.264 and AAC. lib264's license explicitly grants no patent
rights. Distributing source code is generally unaffected. If you build and
distribute devices running this firmware, check whether you need patent
licenses for the codecs you ship.
