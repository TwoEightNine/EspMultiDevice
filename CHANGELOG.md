# Changelog

All notable changes to ESP Media are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the firmware
version follows the rules in
[docs/development.md](docs/development.md#firmware-version).

## [Unreleased]

### Added

- ESP Media is now free software under GPL-3.0-or-later, with third-party
  notices, a security policy, contributing guidelines, and a code of conduct.
- Settings → About shows the copyright and license notice.
- SD app SDK 6 can decode `bitmap` responses, which the 2ch reader example
  uses for thumbnails.
- `sketch.yaml` pins the board package and libraries for reproducible
  `--profile` builds.
- SD app manifests accept `"show_in_home": false` to leave an app out of
  Home, so only other apps open it.
- Settings → Apps enables or disables each SD app on this device; disabled
  apps leave Home and cannot be started.
- SD app SDK 7 adds `esp.apps.status(id)` and `esp.apps.open(id)` to check
  and open other apps, with a launcher example.
- The Audio Player shows the track title, artist, album, and year from
  ID3v1/ID3v2 tags and WAV `INFO`/`id3 ` chunks, and falls back to the file
  name without its extension.
- The Audio Player shows embedded PNG and baseline JPEG cover art, decoded on
  a background task.
- Settings → Benchmark runs a 42-second 3D show (star warp, crystals, a torus
  knot that switches from one core to two, and a plasma tunnel) on both CPU
  cores with live FPS, then shows a score where 1000 is a typical device. The
  best score is kept on the device.

### Changed

- The Audio Player has a "Now playing" layout: an artwork tile, title, artist,
  and album line, a seek bar with elapsed and remaining time (draggable for
  WAV and MP3), previous/next through the track's folder with auto-advance
  and repeat, skipping tracks that fail to open, a volume slider in place of the 0–10
  grid, and a Track info sheet.
- An MP3 starts playing as soon as its tags are read, even on its first
  open. The frame scan for the exact duration and the seek table runs in the
  background.
- The MP3 cache (`.empa`) is now version 2 with a seek table. Version-1 files
  are rebuilt automatically the next time the track is opened.
- The Browser now validates HTTPS certificates against the Mozilla root bundle
  and waits for the clock to sync before loading HTTPS pages.
- The PIN-protected file area is called *hidden files* in the UI, because it
  obscures files rather than encrypting them.
- The display configuration now ships with the project as `tft_setup.h`, so
  TFT_eSPI's global `User_Setup.h` no longer needs editing.
- The build scripts find `arduino-cli` and the ESP32 board package
  automatically and run on Linux and in Git Bash on Windows.

### Fixed

- The Audio Player opens tracks in the hidden-files folder; it used to read
  the encrypted name and report "Unsupported audio file".

### Removed

- The unused arduinoWebSockets library and unused Helix wrapper headers.
- The header symlinks in the sketch root, which broke Windows checkouts.

## [0.2.0] - 2026-10-27

### Added

- SD app SDK 6: permission-gated HTTP(S) requests with streaming JSON parsing,
  plus a 2ch imageboard reader example app.

### Changed

- Faster video playback, and optimized GIF and WebP decoders with animation
  frames cached in PSRAM.
- Faster SD card access, using DMA and SPI clocks of up to 40 MHz.

## [0.1.0] - 2026-10-25

First versioned firmware, with the Settings → About screen showing the
version. It includes the file browser with hidden files, image, audio, video,
and text viewers, recording, the Browser, Wi-Fi tools, USB Drive and Mac
Remote, the optional Assistant, and SD apps up to SDK 5.

[Unreleased]: https://github.com/TwoEightNine/EspMultiDevice/compare/74a76b4...HEAD
[0.2.0]: https://github.com/TwoEightNine/EspMultiDevice/compare/c3a7fc0...74a76b4
[0.1.0]: https://github.com/TwoEightNine/EspMultiDevice/commit/c3a7fc0
