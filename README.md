# ESP Media

ESP Media is an ESP32-S3 media-device project. The current build brings up an 8-bit parallel ILI9488 TFT through TFT_eSPI, polls the display's XPT2046 resistive touch panel through `TouchController`, keeps SD card storage encapsulated in `SdCardController`, and manages station-mode Wi-Fi through `WiFiController`.

All project wiring is kept inside these GPIO ranges:

```text
GPIO 0-21, GPIO 35-42, GPIO 45-48
```

The recommended wiring below avoids `GPIO 0`, `GPIO 19/20`, and `GPIO 45/46` for normal peripherals because those pins are commonly involved in boot strapping or native USB behavior on ESP32-S3 boards.

## Current Features

- Initializes a 3.95 inch ILI9488 TFT in 8-bit parallel mode through TFT_eSPI.
- Polls an XPT2046 touch panel over SPI without using the IRQ pin.
- Mounts an SD card over SPI on pins that do not overlap the TFT parallel bus.
- Runs a simple read/write self-test through `SdCardController`.
- Detects card removal at runtime.
- Retries mounting when a card is inserted again.
- Scans, connects to, disconnects from, and forgets Wi-Fi networks through the Settings flow.
- Remembers Wi-Fi credentials in the ESP32 Wi-Fi configuration and updates the Home status icon.
- Supports an optional card-detect pin for more reliable hot-swap behavior.
- Records 16 kHz mono WAV voice clips from an INMP441 I2S microphone to SD.
- Plays the saved voice clip through a MAX98357A I2S amplifier.
- Streams mono or stereo integer PCM WAV files and local MPEG Layer III MP3 files through the Audio player screen.
- Opens MP4 and 3GP videos with H.264 Baseline/Main/High, H.263, or MPEG-4 Part 2 video at up to 320×240. The first open converts display frames into an SD-backed `.emga` cache before playback; compatible AAC-LC, MP3, and AMR-NB (`samr`, 8 kHz mono) tracks are played through the I2S speaker.
- Includes a simple Browser app with recent sites, HTTP/HTTPS text-page loading, persistent cookies, and fixed page controls.
- Browses SD-card files and directories with folder navigation and pagination.
- Supports a PIN-offset XOR-protected `/.configs` secret folder for media files.
- Uses a lightweight screen/navigation layer so UI screens handle their own drawing, touch, and animation ticks.
- Image Viewer supports BMP, PNG, JPG, JPEG, WebP, and animated GIF/WebP files with PSRAM-backed rendering and SD-backed frame caches. Animated images are downscaled only when needed to fit a 320x240 photo viewport; they are never upscaled. `.emga` v4 stores packed 4,096-color RGB444 pixels per frame, which the second-core loader expands into a PSRAM RGB565 ring. Playback clamps source delays to 100 ms (10 fps) so the SD/touch shared SPI bus has deterministic time to refill.

## Files

Files keeps a bounded in-memory LRU of the directories visited during one Files-screen visit. A folder is enumerated and sorted once (directories first, then case-insensitive natural A–Z); returning to a cached folder does not re-read its entries. The cache survives opening a player or the rename keyboard and is discarded when Files is closed or the SD card changes.

The Files pager is fixed at the bottom of the display with Previous, Page, and Next controls. Regular files have a right-side three-dot menu: Rename opens the keyboard with the complete filename pre-filled, while Delete requires holding its menu action for one second. Folders intentionally have no file-action menu.

Hold the Files tile on Home for one second to enter a four-digit secret PIN. The PIN selects the first byte of `/.configs/index`, a 10,000-byte random key created on first use; it is an offset rather than an authenticated password, so an incorrect value opens the folder with unusable names/content. Secret filenames and source/cache bytes are XOR-encrypted, and filenames are hex-armored for FAT compatibility. While in secret mode, normal files retain their existing actions plus **Move to secret**; secret files themselves have no action menu. This is lightweight obfuscation, not modern authenticated encryption.

Visible BMP, PNG, JPEG, GIF, and WebP files receive 32×32 previews. Decoding runs on the other core and takes the shared SD/touch SPI lock only for individual reads, so list drawing and touch handling remain responsive. Previews are retained in memory for the Files visit and in a validated `.cache/*.eth1` sidecar across visits; the source size and timestamp must match before a sidecar is used. Invalid, unsupported decoder variants, oversized WebP sources, and memory-constrained images retain the normal file icon.

- `esp_media.ino`: application entry point.
- `Screen.h`: base class for application screens.
- `Navigator.h`: screen ID and navigation stack interface.
- `Navigator.cpp`: registered screen lookup and stack transitions.
- `HomeScreen.h`: default screen interface.
- `HomeScreen.cpp`: SD status display and touch-log screen behavior.
- `DisplayController.h`: TFT display controller interface.
- `DisplayController.cpp`: TFT_eSPI setup and simple status screens.
- `TouchController.h`: XPT2046 touch controller interface.
- `TouchController.cpp`: SPI polling, pressure detection, and coordinate mapping.
- `SdCardController.h`: SD card controller interface.
- `SdCardController.cpp`: SD mount, unmount, health check, and file helpers.
- `WiFiController.h` / `WiFiController.cpp`: asynchronous Wi-Fi scan, connection, reconnect, and credential lifecycle.
- `TimeController.h` / `TimeController.cpp`: Wi-Fi-gated asynchronous SNTP synchronization with a fixed UTC+4 local clock.
- `MicController.h`: INMP441 I2S microphone controller interface.
- `MicController.cpp`: microphone I2S setup, power-switch sensing, and PCM reads.
- `SpeakerController.h`: MAX98357A I2S speaker controller interface.
- `SpeakerController.cpp`: speaker I2S setup and PCM writes.
- `VoiceRecorderScreen.h`: voice recorder screen interface.
- `VoiceRecorderScreen.cpp`: record, stop, save-to-SD, and playback UI.
- `FilesScreen.h` / `FilesScreen.cpp`: SD-card file browser UI and navigation.
- `ImageViewerScreen.h` / `ImageViewerScreen.cpp`: full-screen, aspect-fit viewer for BMP, PNG, baseline JPEG, WebP, and GIF images, with reusable `.em16` static canvases and `.emga` animated frame caches.
- `AudioPlayerScreen.h` / `AudioPlayerScreen.cpp`: streamed WAV and MP3 player with play/pause controls and read-only progress.
- `VideoPlayerScreen.h` / `VideoPlayerScreen.cpp`: MP4/3GP H.264, H.263, and MPEG-4 Part 2 converter and cached video player with optional AAC-LC/MP3/AMR-NB audio.
- `Navigation.h`: reusable payload and result envelopes for screen-to-screen data exchange.
- `FileFormats.h`: shared registry for image, audio, and video media plus explicit internal-only cache formats.
- `WiFiScreen.h` / `WiFiScreen.cpp`: Wi-Fi enable switch, connection details, scan list, and password flow.
- `BrowserScreen.h` / `BrowserScreen.cpp`: lightweight HTTP/HTTPS browser, HTML-to-text renderer, bounded cookie jar, recent-site persistence, and browser UI.

## Browser

The Home Browser app stores up to eight manually entered sites and shows as many as fit above the footer on its start page. Tap **Open URL** to enter a URL with the on-screen keyboard; a URL without a scheme is treated as `https://`. Following hyperlinks, reopening a recent site, and navigating browser history do not create or reorder recent-site entries. The footer uses icon-only controls for back, home/start, page up, and page down. While a page or its image previews are loading, the back position becomes an **X** that cancels the active work; cancelling before readable content arrives shows `Loading cancelled`. Foreground page and direct-media transfers show their current phase plus a progress bar; a percentage is shown when the server sends `Content-Length`, otherwise the received byte count is shown. The Home button is disabled on the start page. Back history is bounded to the current Browser session; it restores a cached text page immediately when available, including cached embedded-image sources, and otherwise re-fetches the previous URL. Fresh URL opens, recent sites, links, and redirects are always network-first. The session cache is stored under `/.system/browser_session/`, has a 16 MiB least-recently-used limit shared by pages and inline-image sources, is cleared when Browser closes, and is removed on the next Browser start after an interrupted session. It is not persisted across Browser sessions. Recent entries and rendered HTTP/HTTPS hyperlinks are tappable. The browser resolves absolute, protocol-relative, root-relative, directory-relative, and query-relative link targets against the current page, then follows normal HTTP redirects. Direct supported media responses—BMP, PNG, JPEG, GIF, WebP, WAV, and MP3—are streamed into `/.system/browser_media/` and opened in the Image Viewer or Audio Player. Downloads are limited to 16 MiB each and remain on the SD card until removed externally. Fragment-only links and non-web schemes such as `mailto:`, `tel:`, and `javascript:` remain non-tappable.

The browser supports `http://` and `https://` GET requests, up to five redirects, chunked or fixed-length uncompressed responses, and a bounded cookie jar. It retains up to 512 KiB from a response rather than rejecting a large page, then renders its readable static text as paged styled rows with 22-pixel leading. Web documents default to a white background and black text; the Browser app bar, start page, and footer retain their dark UI. Supported media downloads are written through a temporary file, signature-checked, and discarded if malformed, interrupted, compressed, or too large. `FileFormats.h` is the single registry used by Browser and Files for image, audio, and video extensions and MIME types; MP4 and 3GP downloads are verified as ISO-BMFF files and opened in the Video player. `.em16`, `.emga`, and `.empa` are internal-only caches rather than downloadable or openable source media. Inline HTML color and `bgcolor` plus inline or embedded CSS `color`, `background`/`background-color`, `font-weight`, `font-style`, and `text-decoration: underline` are supported; colored block elements fill the readable document width, while inline backgrounds cover their own content. Embedded CSS accepts up to 32 simple element, `.class`, or `#id` rules (including comma-separated selector lists), with selector specificity/source order followed by inline styles. It ignores external stylesheets, combinators, pseudo-classes, attribute selectors, media queries, CSS variables, and unsupported or malformed rules. `display: block` and `width: 100%` apply only to image placeholders, making them full-width; they work from HTML attributes, inline CSS, or matching embedded CSS. Unsupported embedded images use a compact icon-and-`alt` placeholder; they remain inline and wrap-content by default, and are blue and tappable inside supported HTTP(S) links. `audio`, `video`, and `source` tags with an HTTP(S) `src` are rendered as tappable media links. Cyrillic text is temporarily transliterated to Latin. Pages that contain only JavaScript report that limitation; embeds, and forms appear as text stubs; form placeholders include the control tag and its `name` (or `id`, accessible label, action, or input type fallback). HTTP Basic authentication and form login are not supported.

Embedded baseline JPEG, non-interlaced PNG, and GIF images are queued after the static page text has rendered, then downloaded and decoded by a worker on the other CPU core. Completed previews are handed back to the UI task, which redraws the active document area without refreshing the header or footer controls. Each source has a 384 KiB limit and renders aspect-fit no larger than 200×200 pixels; smaller sources are never upscaled. GIF thumbnails contain only their first frame. Fresh pages retain supported image sources in the browser session cache; restoring that page through Back decodes those local sources instead of downloading them again, waits for the previews to finish, and then draws the whole page once to avoid flicker. The browser keeps rendering images until another thumbnail allocation cannot fit in memory, then leaves the remaining images as lightweight placeholders. Every image source URL remains tappable, opening the original in the Image Viewer without a link outline. Other formats and oversized images retain the placeholder.

Recent sites and cookies are stored as plaintext in `/.system/browser_state.txt` on the SD card. Browser operation continues without an SD card, but no data persists. HTTPS certificate validation is deliberately disabled for this initial compatibility-focused version: do not use the browser for sensitive accounts or private information.

## UI Kit

`UiKit.h/.cpp` is the shared drawing contract for navigable application screens. Use it instead of duplicating header, list-row, ellipsis, icon-scaling, or navigation-hit-test code. Home, Splash, and the full-screen Image Viewer are intentional exceptions; the viewer uses only the kit's close affordance.

### App bars

Draw an app bar after clearing the screen and before drawing content:

```cpp
#include "UiKit.h"

UiKit::drawTopAppBar(_display.tft(), _display.width(), F("My screen"));
```

The helper draws a 48 px `#111` bar, a white 40×40 back icon, and a left-aligned font-4 title. The title begins at x=56, is visually centered vertically (including the font-4 baseline correction), and ellipsizes before the 16 px trailing margin. Pass `UiKit::NavigationIcon::Close` as the fourth argument for a dismiss action such as text entry.

Reserve y=0–47 for the bar. The navigation target is deliberately the entire leading corner before the title (x=0–55, y=0–47), not just the icon:

```cpp
if (UiKit::navigationIconContains(point.x, point.y)) {
  _navigator.pop();
  return;
}
```

### List cells

Use `UiKit::drawListCell()` for each standard row. It accepts the display width, row y-coordinate, primary label, and optional 16×16 monochrome XBM icon plus color:

```cpp
const int16_t rowY = UiKit::AppBarHeight + 16;
UiKit::drawListCell(tft, _display.width(), rowY, F("Wi-Fi"));
UiKit::drawListCell(tft, _display.width(), rowY + UiKit::ListCellHeight,
                    fileName, ic_file, TFT_LIGHTGREY);
```

Each cell is 40 px high. Labels use font 2; optional icons are scaled to 32×32 and vertically centered. Text starts at x=16 without an icon or x=56 with one. The 1 px dark-gray divider begins at that same text x-coordinate and ends 16 px from the right edge. Labels ellipsize to preserve trailing metadata; the optional final `trailingText` argument renders compact, right-aligned detail such as Wi-Fi security and RSSI.

The drawing helper does not install click handlers. The owning screen must make the **entire cell** tappable—x=0 through the display width and y through `rowY + UiKit::ListCellHeight`—rather than limiting hits to the label or icon. Recalculate pagination and lower content layout from `UiKit::ListCellHeight`; do not copy an old row-height constant.

`drawTopAppBar()` and `drawListCell()` restore TFT_eSPI's default free-font and top-left text datum. Set any specialized font, datum, or color after calling a helper.

## Screen Architecture

`esp_media.ino` owns hardware setup and screen registration only. After a screen is registered, the sketch does not call methods on specific screen types.

The main loop talks only to the generic active `Screen` from `Navigator`:

- changed touch points are forwarded through `Screen::handleTouch()`;
- non-blocking screen work runs through `Screen::tick()`.

Screens get common UI services from the base `Screen` constructor: `Navigator`, `TouchController`, and `DisplayController`. Screens that need feature-specific services receive those controllers in their own constructors. For example, `HomeScreen` receives `SdCardController&` and owns SD status drawing, mount-change handling, and the read/write self-test display. `VoiceRecorderScreen` receives `SdCardController&`, `MicController&`, and `SpeakerController&`, then owns the recorder UI and audio file lifecycle.

`Navigator::push` and `Navigator::replace` optionally accept a `ScreenData` payload, which is retained with the stack entry and delivered to the destination in `Screen::onEnter(const ScreenData&)`. A child can return a `ScreenResult` with `Navigator::pop(result)`; the resumed screen receives it through `Screen::onResult`. The Files screen uses this mechanism to pass image, audio, and video paths to their players.

## Image Decoder Libraries

Install the `PNGdec` and `JPEGDEC` libraries from the Arduino Library Manager before compiling. Animated GIF decoding is pinned in `third_party/AnimatedGIF` and is compiled with the sketch; it retains the upstream Apache-2.0 notice. WebP decoding is pinned in `third_party/libwebp` and is compiled as a portable, decode-only C subset; its BSD license and source version are retained beside the vendored sources. MP3 decoding uses the bundled public-domain `third_party/minimp3` implementation. Video support pins minimp4 (CC0), the generic-C lib264 H.264 decoder (BSD-3-Clause-Clear, with its upstream AVC patent notice), a minimized FFmpeg 9.0.1 H.263/MPEG-4 Part 2 decoder subset (LGPL-2.1-or-later; see `third_party/ffmpeg/README.esp-media.md`), the Apache-2.0 OpenCORE AMR 0.1.6 decoder-only AMR-NB subset (see `third_party/opencore-amr/README.esp-media.md`), and the Helix AAC decoder (RPSL/GPL wrapper); their notices are retained under `third_party/`. The included `build_opt.h` raises PNGdec's row-buffer limit for PNG images up to 4096 pixels wide. The viewer accepts static, non-interlaced PNG images, baseline JPEG images, simple and extended WebP files (including lossless and alpha), and GIF87a/GIF89a or animated WebP files with animation, transparency, disposal/blending, and loop metadata. Very large animated images may still be rejected when their decoder canvas and source buffer cannot fit in currently available PSRAM. Animated PNG and progressive JPEG files remain unsupported.

For reproducible legacy-video regression fixtures, run `sh tools/generate_legacy_video_fixtures.sh` with FFmpeg 9.0.1; it writes 176×144, 10 fps H.263/3GP and MPEG-4 Part 2/MP4 clips under `tests/video/fixtures/`.

## Image Cache Formats

The viewer creates SD-card caches beside each source image in a `.cache` directory. For example, `/photos/beach.jpg` becomes `/photos/.cache/beach.jpg.em16`, and `/photos/waves.gif` becomes `/photos/.cache/waves.gif.emga`. A cache is first written as the same path with a `.tmp` suffix and then renamed only after it is complete. Invalid, incomplete, or obsolete caches are rebuilt automatically.

The audio player similarly stores a tiny MP3-analysis sidecar. For example, `/music/song.mp3` becomes `/music/.cache/song.mp3.empa`. It records the validated audio bounds, format, and total decoded samples, allowing future plays to skip the full frame scan while continuing to decode the original compressed MP3 during playback. The cache is accepted only when the source size and last-modified timestamp match.

### `.em16` — static-image canvas cache

`.em16` is used for BMP, PNG, and JPEG images. It is an `EM16` version-1 file with a 24-byte little-endian header followed by a fixed 480 × 320 RGB565 canvas (307,200 bytes).

- Header fields: magic (`EM16`), version, pixel encoding (`0` = little-endian RGB565), canvas width and height, source-file byte size, source last-modified time, and pixel-payload size.
- The canvas contains the viewer's aspect-fit render, including its black letterbox area, so cache playback needs no image decoding.
- A cache is accepted only when its source size, modification time, cache geometry, payload size, and file length all match the current viewer.

### `.emga` — animated GIF/WebP frame cache

`.emga` is used for animated GIF and WebP sources. The current format is `EMGA` version 4 with a 40-byte little-endian header and fixed-size records for each frame. Older `.emga` versions are deliberately invalidated and regenerated.

- Header fields: magic (`EMGA`), version, pixel encoding (`2` = packed RGB444), 480 × 320 display geometry, source-file byte size, source last-modified time, source GIF width and height, frame count, loop count, and the cached draw width and height.
- Every frame record begins with a 32-bit little-endian display delay in milliseconds. The remaining bytes are two 12-bit RGB444 pixels packed into three bytes (the final odd pixel uses two bytes). This preserves 4,096 colors while using 25% less SD storage and read bandwidth than RGB565.
- GIFs are centered in a maximum 320 × 240 viewport. Sources larger than that are downscaled; smaller sources retain their native size and are never upscaled.
- During playback, the loader on the other CPU core reads and expands RGB444 records into RGB565 slots in a PSRAM ring. The TFT receives those RGB565 pixels, while the SD cache stays compact enough to sustain the shared SD/touch SPI bus.

## ESP32-S3 N16R8 Playback Profile

Select an ESP32-S3 board profile with 16 MiB flash, 8 MiB PSRAM, and a 240 MHz CPU. Use an application partition of at least 1.5 MiB; for an N16R8, **16M Flash (3MB APP/9.9MB FATFS)** is a known-good choice. The default 1.2 MiB application partition is too small for this firmware. Startup logs the detected CPU, flash, PSRAM total, free PSRAM, and the largest allocatable PSRAM block. GIF playback requires at least three 480x320 RGB565 PSRAM frames plus a 512 KiB reserve; lower-memory profiles show an image-viewer error rather than falling back to synchronous SD reads.

## TFT_eSPI Setup

TFT_eSPI is configured by the library-level `User_Setup.h`, not by the sketch folder. For best 8-bit parallel performance on ESP32-S3, keep `TFT_D0` through `TFT_D7`, `TFT_WR`, and `TFT_DC` below GPIO 32. This setup keeps the whole TFT bus in `GPIO 4-16` and moves SD to `GPIO 39-42`.

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

// No TFT_BL is defined because this module has no separate BL control pin.
```

To update TFT_eSPI:

1. Open:

```text
/Users/fkdplc/Documents/Arduino/libraries/TFT_eSPI/User_Setup.h
```

2. In the driver section, make sure only this driver is active:

```cpp
#define ILI9488_DRIVER
```

3. In the ESP32 parallel pin section, replace the old pin block with the block above.
4. Save `User_Setup.h`, then rebuild and upload the sketch.

## Default TFT Wiring

The display uses 8-bit parallel mode. Do not connect the TFT `SDO`/`MISO` pin to the SD card SPI bus; many ILI9488 boards do not release MISO cleanly when chip-select is inactive.

| TFT Module Pin | ESP32-S3 GPIO |
| --- | --- |
| `DB0` / `D0` | `GPIO 4` |
| `DB1` / `D1` | `GPIO 5` |
| `DB2` / `D2` | `GPIO 6` |
| `DB3` / `D3` | `GPIO 7` |
| `DB4` / `D4` | `GPIO 8` |
| `DB5` / `D5` | `GPIO 9` |
| `DB6` / `D6` | `GPIO 10` |
| `DB7` / `D7` | `GPIO 11` |
| `WR` | `GPIO 12` |
| `RS` / `DC` | `GPIO 13` |
| `RD` | `GPIO 14` |
| `CS` | `GPIO 15` |
| `RST` | `GPIO 16` |
| `VCC` | Match your module, usually `3.3V` or `5V` |
| `GND` | `GND` |

`GPIO 4-16` is the preferred TFT group because TFT_eSPI can write the 8-bit bus and the write strobe through the lower GPIO register path. The data pins do not need to be physically consecutive for TFT_eSPI, but using a tidy run makes wiring and debugging much easier.

If the display has a pin labelled `LED`, `LED_A`, `A`, or similar, that is usually raw backlight power rather than a logic control input. Connect it to the module's required backlight supply, not to a GPIO.

## Default SD Card Wiring

The sketch uses SPI mode, not SDMMC mode. It runs the shared SD/touch bus at a conservative 20 MHz because long animation reads were not reliable at 40 MHz with the current wiring. The GIF loader reads 16 KiB sequential blocks on the second core and releases the SPI lock between blocks for touch polling.

| SD Module Pin | ESP32-S3 GPIO |
| --- | --- |
| `SCK` / `CLK` | `GPIO 40` |
| `MISO` / `DO` | `GPIO 39` |
| `MOSI` / `DI` | `GPIO 41` |
| `CS` / `SS` | `GPIO 42` |
| `VCC` | Match your SD module, usually `3.3V` or `5V` |
| `GND` | `GND` |

If your wiring is different, update these constants in `esp_media.ino`:

```cpp
static const int SD_SCK_PIN = 40;
static const int SD_MISO_PIN = 39;
static const int SD_MOSI_PIN = 41;
static const int SD_CS_PIN = 42;
```

## Default Touch Wiring

The XPT2046 touch controller shares the module's external SPI clock/data lines with the SD card and has its own chip-select pin. On this display, `EX_CLK` is the shared external SPI clock for both SD and touch. Do not connect `TP_IRQ` right now; the sketch polls touch state instead.

| Touch Module Pin | ESP32-S3 GPIO |
| --- | --- |
| `EX_CLK` | `GPIO 40` |
| `MISO` | `GPIO 39` |
| `MOSI` | `GPIO 41` |
| `TP_CS` | `GPIO 38` |
| `TP_IRQ` | Not connected |
| `VCC` | Match your module, usually `3.3V` |
| `GND` | `GND` |

If your wiring is different, update these constants in `esp_media.ino`:

```cpp
static const int TOUCH_SCK_PIN = SD_SCK_PIN;
static const int TOUCH_MISO_PIN = SD_MISO_PIN;
static const int TOUCH_MOSI_PIN = SD_MOSI_PIN;
static const int TOUCH_CS_PIN = 38;
static const int TOUCH_IRQ_PIN = TouchController::NoIrqPin;
```

Touch pressure is learned by the Touch screen calibration flow and stored in `/.system/touch` on the SD card. Before calibration, the controller uses its conservative built-in noise floor.

## Default INMP441 Microphone Wiring

The INMP441 uses I2S input on pins that avoid the TFT, SD, touch, and the ESP32-S3 high GPIOs that are commonly tied to flash, PSRAM, or board features. The microphone power should go through a real physical switch. Power-switch sensing is supported by `MicController`, but it is disabled in the default wiring to avoid touching extra board-dependent pins during startup.

| INMP441 Pin / Switch Node | ESP32-S3 GPIO / Supply |
| --- | --- |
| `SCK` / `BCLK` | `GPIO 18` |
| `WS` / `LRCL` | `GPIO 21` |
| `SD` / `DOUT` | `GPIO 17` |
| `VDD` | Switched `3.3V` from the physical mic power switch |
| Switch sense, same switched `3.3V` node | `GPIO 48` |
| `L/R` | `GND` for left-channel output |
| `GND` | `GND` |

If you do not wire the switch sense line, leave `GPIO 48` disconnected and set `MIC_POWER_SENSE_PIN` to `MicController::NoPowerSensePin`. The mic will still record, but the UI cannot tell whether the physical switch is off.

If your wiring is different, update these constants in `esp_media.ino`:

```cpp
static const int MIC_BCLK_PIN = 18;
static const int MIC_WS_PIN = 21;
static const int MIC_DATA_PIN = 17;
static const int MIC_POWER_SENSE_PIN = 48;
```

## Default MAX98357A Speaker Wiring

The MAX98357A shares the I2S clock and word-select pins with the microphone, but recording and playback are never active at the same time. This avoids the risky high GPIO range and keeps the existing SPI wiring unchanged.

| MAX98357A Pin | ESP32-S3 GPIO / Supply |
| --- | --- |
| `BCLK` | `GPIO 18` |
| `LRC` / `WS` | `GPIO 21` |
| `DIN` | `GPIO 1` |
| `VIN` | `5V` preferred if your module and power budget allow it, otherwise `3.3V` |
| `GND` | `GND` |
| `SD` / `EN` | `GPIO 2`, with a 100k pulldown to `GND` recommended |
| `GAIN` | `100k` resistor to `GND` for highest gain, 15 dB |
| Speaker `+` / `-` | 4-8 ohm speaker terminals only |

Do not connect either speaker output terminal to ground. The MAX98357A drives the speaker differentially. Do not tie `SD` / `EN` permanently to `VIN` for this project; the sketch uses `GPIO 2` to keep the amplifier muted during boot and recording, then enables it only for playback.

If your wiring is different, update these constants in `esp_media.ino`:

```cpp
static const int SPEAKER_BCLK_PIN = MIC_BCLK_PIN;
static const int SPEAKER_WS_PIN = MIC_WS_PIN;
static const int SPEAKER_DATA_PIN = 1;
static const int SPEAKER_ENABLE_PIN = 2;
```

## 5V Power Notes

The system has three loads with very different current profiles: ESP32-S3/Wi-Fi bursts, a continuously lit TFT backlight, and the MAX98357A's audio peaks. A supply's advertised `2 A` value does not establish that it can keep its voltage above the ESP32 board regulator's input requirement through those peaks. USB leads, breadboards, Dupont wires, battery-protection boards, boost converters, and the ESP32 board's `VIN` path each add resistance, so the voltage at the ESP32 can be much lower than the voltage measured at the supply.

Use one regulated power rail with at least `5 V / 3 A` peak capability at the device, not one rail daisy-chained through an ESP32 pin. Make a star connection at the regulator output: separate positive and ground pairs go to the ESP32 board, TFT/SD module, and MAX98357A. Use short, thick wires for the amplifier and display; do not route their return current through a breadboard or through the ESP32 `GND` pin/wires.

Put the following capacitors physically close to the loads, observing polarity:

- `470–1000 uF`, low-ESR, rated at least 6.3 V, plus `100 nF` ceramic across MAX98357A `VIN` and `GND`.
- `220–470 uF`, low-ESR, rated at least 6.3 V, plus `100 nF` ceramic across the TFT module supply and `GND`.
- `100–220 uF`, rated at least 6.3 V, plus `100 nF` ceramic across the ESP32 board's 5 V input and a nearby `GND` pin.

The capacitors suppress short transients; they cannot compensate for an undersized converter, a weak battery, or thin/long wiring.

For a one-cell Li-ion battery, a TP4056 is only a charger/protection board—it does **not** produce 5 V and it has no load-sharing power path. Use a protected cell (or separate protection/BMS), a charger with power-path/load-sharing, and a synchronous 5 V boost converter rated for at least `3 A` output peak. Its battery-side peak current will exceed `4.5 A` at 5 V / 3 A output once conversion losses and a low cell voltage are included, so a 14500 cell, small power-bank module, or ordinary breadboard wiring is usually unsuitable. A 2S battery with the appropriate BMS/charger followed by a 5 V buck converter is often the more robust architecture.

Feed the ESP32 board only through the input its exact board schematic documents for a regulated 5 V supply (`5V`/`VBUS`/`VIN` varies by board). Feed the TFT/SD module and MAX98357A from their documented supplies; many SD/touch boards are 3.3 V logic devices even when their `VCC` pin accepts 5 V. All grounds must meet at the common star point.

Do not connect an external `5V` supply to the ESP32-S3 board's `VIN` while USB is connected unless your exact board documents that simultaneous `VIN` and USB power is supported. Many boards have power-path protection, but many small ESP32-S3 boards do not. The safer default is:

- USB powers the ESP32-S3 board for programming and serial.
- External `5V` powers the TFT and MAX98357A.
- All grounds are common.
- The external `5V` positive line does not feed back into USB `5V` or board `VIN`.

The two-USB arrangement may appear stable because one port is feeding the ESP32 while the other feeds the peripherals, but it must not be used as the final design unless the board schematic proves the two 5 V paths are isolated. It can back-power a USB port or make current sharing unpredictable.

### Power fault test

The sketch prints `Reset cause:` near the start of every serial boot. Capture the complete boot message at 115200 baud. `brownout reset (supply voltage sag)` confirms the diagnosis; a watchdog or panic message instead points to firmware and should be investigated separately.

Test using the intended single supply and repeat each stage long enough to reproduce the intermittent failure:

1. ESP32 alone, then ESP32 plus the display, then add the SD card.
2. Turn Wi-Fi on and run repeated scans; this creates high ESP32 transmit/receive bursts.
3. Play a bass-heavy or continuous tone at volume 3, 5, then 10. If only this stage resets, the amplifier rail/wiring is the immediate problem.
4. Measure the 5 V rail at the ESP32 input and at the MAX98357A `VIN`/`GND` pins while the failing workload runs. A scope is best; a multimeter's MIN/MAX mode is still useful. Measure ground-to-ground voltage drop too.

For a quick isolation test, unplug only the MAX98357A `VIN` (leave its logic wires disconnected or its `SD`/`EN` low) and repeat. If the resets stop, do not solve it in firmware: upgrade the power rail, wiring, and local bulk capacitance before reconnecting the amplifier.

## Pin Allocation

| Purpose | GPIOs |
| --- | --- |
| TFT data bus | `GPIO 4-11` |
| TFT control | `GPIO 12-16` |
| SD SPI | `GPIO 39-42` |
| Touch SPI | Shares `GPIO 39-41`, plus `GPIO 38` for `T_CS` |
| INMP441 mic I2S | `GPIO 18`, `GPIO 21`, `GPIO 17` |
| Mic power switch sense | `GPIO 48` |
| MAX98357A speaker I2S | Shares `GPIO 18`, `GPIO 21`, plus `GPIO 1` |
| MAX98357A speaker enable | `GPIO 2` |
| Available from allowed set | Confirm per board before using high GPIOs |
| Usually best left unused unless needed | `GPIO 0`, `GPIO 19`, `GPIO 20`, `GPIO 45`, `GPIO 46` |

## Runtime Eject and Insert

`SdCardController::update()` records an event when the SD card state changes; the active SD screen receives it through `takeEvent()`:

- `SdCardEvent::Mounted`: a card was mounted.
- `SdCardEvent::Removed`: the mounted card was removed or stopped responding.
- `SdCardEvent::MountFailed`: a mount retry failed.
- `SdCardEvent::None`: no state change.

The main loop polls `SdCardController::update()` independently of the active screen, so ejecting a card is detected while any screen is open. SD-owning screens call `takeEvent()` from their non-blocking `tick()` methods to react to the pending transition.

For best hot-swap detection, use a socket or module with a card-detect pin and set:

```cpp
static const int SD_CARD_DETECT_PIN = -1;
```

to the GPIO connected to the socket's `CD` pin. Most card-detect switches are active-low, which matches the controller default.

If no card-detect pin is available, leave `SD_CARD_DETECT_PIN` as `-1`. The controller retries initialization every three seconds after a failed mount, allowing a card inserted after a runtime eject to mount automatically. A physical card-detect pin remains the most reliable way to support automatic eject/insert handling.

Even without a card-detect pin, a mounted card is probed with a small raw-sector read every 500 ms, so a runtime eject is reported as `Removed` instead of leaving the filesystem marked mounted.

## SD Card Formatting

Use FAT32 with a Master Boot Record partition map.

On macOS, if Disk Utility does not show a `Scheme` option, choose `View > Show All Devices`, then select the top-level physical SD card instead of the indented volume.

If the `Scheme` option still does not appear, use Terminal carefully:

```sh
diskutil list
```

Find the SD card disk, such as `/dev/disk4`. Then erase the whole card as FAT32 with a Master Boot Record partition map:

```sh
diskutil eraseDisk FAT32 ESPMEDIA MBRFormat /dev/disk4
```

Replace `/dev/disk4` with the actual SD card disk from `diskutil list`. This erases the selected disk completely.
