# Development, dependencies, and third-party code

## Build inputs

Open `esp_media.ino` as an Arduino sketch. Arduino recursively compiles `src/`; `build_opt.h` supplies `PNG_MAX_BUFFERED_PIXELS=32832` and include paths for the bundled codec subsets.

The standard profile is Arduino-ESP32 3.3.8, **ESP32S3 Dev Module**, 16 MB flash, OPI PSRAM, Custom partition scheme, and 240 MHz CPU. Select USB-OTG (TinyUSB), disable CDC/Firmware MSC/DFU On Boot, and use UART0 / Hardware CDC upload mode. `Serial` consequently resolves to UART0. Run `tools/build_esp32s3.sh` for the reproducible build. It selects that complete profile and overrides Arduino's overly broad generic Custom maximum with the actual application-slot limit of 6,553,600 bytes, then independently checks the emitted binary.

`partitions.csv` retains NVS at `0x9000`/`0x5000` and OTA data at `0xe000`/`0x2000`, followed by two `0x640000` OTA slots, `0x360000` SPIFFS, and `0x10000` core dump. Flash the partition table when migrating from the old 3 MB/9.9 MB FATFS profile. Preferences/NVS remains compatible because its location and size do not change; internal FATFS content is not preserved or used by ESP Media.

Install `TFT_eSPI`, `PNGdec`, and `JPEGDEC` through Arduino Library Manager. The TFT setup is a library-level configuration described in [hardware.md](hardware.md), not a source change in this repository.

## Bundled dependencies

The source trees below are pinned under `third_party/`, retained with their notices, and compiled only through project shims:

| Dependency | Project role | License / source note |
| --- | --- | --- |
| AnimatedGIF | GIF decoding | Upstream Apache-2.0; version in `third_party/AnimatedGIF/VERSION.txt` |
| libwebp | decode-only WebP subset | BSD; version and copying notice retained beside source |
| minimp3 | MP3 decoding | Public-domain implementation |
| minimp4 | MP4 demuxing | CC0-1.0, pinned revision in `third_party/VIDEO_DEPENDENCIES.md` |
| lib264 | H.264 decoding | BSD-3-Clause-Clear with upstream AVC patent notice; generic C only |
| FFmpeg 9.0.1 subset | H.263/MPEG-4 Part 2 decoding | LGPL-2.1-or-later; details in `third_party/ffmpeg/README.esp-media.md` |
| OpenCORE AMR 0.1.6 subset | AMR-NB decoding | Apache-2.0; details in `third_party/opencore-amr/README.esp-media.md` |
| Helix AAC | AAC decoding | RPSL/GPL wrapper; notices in `third_party/libhelix/` |
| arduinoWebSockets 2.7.2 client subset | Stage 0 compatibility evaluation only; retained reproducibly but not the runtime assistant transport | LGPL-2.1 plus bundled libb64 notice; source URL and SHA-256 in `third_party/arduinoWebSockets/VERSION.txt` |
| 78/esp-opus 1.0.5 mono subset | future assistant Opus encode/decode | Component metadata says MIT; contained codec terms and patent notices are retained in `COPYING` and `LICENSE_PLEASE_READ.txt`; source URL, commit, and SHA-256 in `third_party/esp-opus/VERSION.txt` |

Do not casually upgrade, reconfigure, or copy a dependency into the Arduino libraries directory. A change can alter compilation boundaries, binary size, codec behavior, and license obligations. Keep portable generic-C video paths; architecture-specific SIMD sources are deliberately excluded. The Opus subset is fixed-point with float APIs disabled, uses the ESP32-S3 Xtensa LX7 definitions and `-O2`, and excludes tests, tools, demos, DNN/DRED, multistream/projection implementations, floating-point SILK, and non-Xtensa platform sources. Its quoted cross-directory includes carry the path-only Arduino portability patch recorded in `third_party/esp-opus/README.esp-media.md`; do not replace them with working-directory-dependent `build_opt.h` include flags. `arduinoWebSockets` remains vendor-pinned for the dependency spike and license record, but Stage 0 did not certify it as the runtime transport on Arduino-ESP32 3.3.8; see its local README for the evaluation notes.

Arduino-ESP32's existing `HTTPClient`, `NetworkClientSecure`, `Preferences`, and IDF `cJSON` are the selected platform facilities for future assistant HTTP, TLS, storage, and JSON work. Do not add parallel libraries. Assistant WSS code must use the project `SecureWebSocketClient`, which implements the small, bounded RFC 6455 client layer over `NetworkClientSecure` and requires a non-empty trust input. Its TLS-handshake allocations are directed to PSRAM during connection setup to preserve the internal-heap contiguous block. Do not reuse `BrowserTransport` or call arduinoWebSockets directly.

The hosted Stage 1 bootstrap and WSS probe trust the bundled DigiCert Global
Root G2 PEM, sourced from DigiCert with SHA-256 fingerprint
`CB:3C:CB:B7:60:31:E5:E0:13:8F:8D:D3:9A:23:F9:DE:47:FF:C3:5E:43:C1:14:4C:EA:27:D4:6A:5A:B1:CB:5F`.
The observed `api.tenclass.net` and `api.xiaozhi.me` chains both terminated at
that root. Treat a future certificate-chain failure as a trust-input review;
never introduce an insecure fallback.

## Stage 0 measurements and probes

The pre-change baseline was recorded with Arduino-ESP32 3.3.8 and the legacy 3 MB application profile:

| Image | Firmware | Static DRAM | Result |
| --- | ---: | ---: | --- |
| Existing application, legacy 3 MB slot | 2,757,290 bytes | 70,584 bytes | 87% of 3,145,728 bytes |
| Existing application plus Stage 0 source, custom 6.25 MB slot | 2,759,930 bytes | 70,584 bytes | 42% of 6,553,600 bytes; normal assistant behavior remains disabled |
| Dependency-linked diagnostic (USB CDC probe profile, configured local WSS fixture) | 3,025,632 bytes | 72,864 bytes | 46% of the 6.25 MiB slot; 3,527,968 bytes remain and the image is below the 4.5 MB gate |

Build the dependency-linked diagnostic with `tools/build_assistant_dependency_probe.sh`. The compile-time diagnostic retains the complete application in the linked image but runs only `tests/assistant_dependency_probe/assistant_dependency_probe.ino`; normal builds do not include that code. It uses the same two-port profile as production, with hardware results printed through UART0 on the USB-to-UART connector. The fixed-point codec work runs in a dedicated 48 KiB diagnostic task rather than Arduino's smaller `loopTask`; transport/codec heap-growth measurements begin inside that task after its stack is allocated. With no local configuration it benchmarks deterministic PCM over 1,000 frames at each decoder rate and checks the missing-CA failure. The configured local WSS suite synchronizes time through the existing SNTP service before certificate validation; its Wi-Fi network must permit DNS and NTP/UDP 123. Follow `tests/assistant_dependency_probe/README.md` to run the local TLS echo fixture and enable the valid/wrong CA, custom-header, 1,000 text plus 1,000 binary round trips, and 20 reconnect checks. It never contacts Xiaozhi.

The same Stage 0 production source was also compiled with the legacy profile's explicit 3,145,728-byte ceiling: 2,759,930 bytes (87%) and 70,584 bytes static DRAM. The project `partitions.csv` remains the flashed layout; this legacy compile is a compatibility/size gate, not the standard upload configuration.

Record the complete 115200-baud Serial result here after a physical-device run:

| Runtime gate | Recorded result |
| --- | --- |
| Free/largest internal heap at start, Wi-Fi-ready, active WSS, and final | 189,500 / 163,828 bytes; 134,316 / 122,868 bytes; 133,560 / 122,868 bytes; 135,992 / 122,868 bytes |
| Free PSRAM at start, active WSS, and final | 8,386,096; 8,325,148; 8,374,740 bytes |
| Permanent internal-heap growth after transport and codecs | 2,292 bytes (2,244 bytes transport, 0 bytes codec initialization) |
| Dedicated 48 KiB probe-task stack high-water mark | 23,348 bytes remaining |
| 16/24 kHz encode/decode sample counts and maximum packet | 1,000 frames each; exact 960 / 1,440 decoded samples; 194-byte maximum packet |
| P95 encode/decode timings | At most 22,600 / 2,334 µs (encode / decode), under the 30 / 20 ms gates |
| Missing and wrong CA rejection | Passed (failed closed) |
| Valid CA, required header, 1,000 text/binary echoes, and 20 WSS cycles | Passed against the local TLS fixture; no monotonic heap loss reported |

The physical-device Stage 0 report passed every threshold: codec/TLS initialization growth below 32 KiB internal heap, a 122,868-byte largest internal block after active WSS testing, no monotonic reconnect/frame loss, a 194-byte maximum Opus packet, exact 960/1,440 decoded samples, and P95 encode/decode below 30/20 ms. Stage 1 may begin, but it must add its session loop off the UI task and retain the documented half-duplex audio ownership.

## Stage 1 hosted compatibility probe

Build the Xiaozhi-hosted diagnostic with
`tools/build_xiaozhi_compatibility_probe.sh`. It retains the complete
application in the linked image while the compile-time entry point bypasses
normal setup. It uses the production two-port USB profile, so capture UART0 at
115200 baud through the USB-to-UART connector. The diagnostic requires saved
Wi-Fi credentials plus DNS and NTP access and never initializes the display,
audio controllers, SD card, or USB mass storage.

The implementation follows Xiaozhi upstream commit
`ac6deed3d8e75348475364bf40ad953c6cd48054`: activation version 1, the official
bootstrap headers and board report, dynamic WSS credentials, protocol-version
header, and client/server hello exchange. Firmware and MQTT instructions in the
response are deliberately ignored. The build enforces both the 6,553,600-byte
application slot and a 4.5 MiB diagnostic gate.

The implementation build verified at 2,944,656 bytes. Its companion regression
builds also passed: normal firmware at 2,832,960 bytes and the Stage 0 assistant
dependency diagnostic at 3,102,032 bytes, all within the 6,553,600-byte slot.

Record the complete redacted Serial result after account binding and the
automatic two-boot hardware run:

| Runtime gate | Recorded result |
| --- | --- |
| Stable MAC-derived Device-Id and NVS UUID across reboot | Pending hardware run |
| Normal Xiaozhi account binding | Pending hardware run |
| Non-placeholder bootstrap WSS credentials | Pending hardware run |
| Hosted WSS upgrade and valid server hello | Pending hardware run |
| MCP disabled or omitted acceptance | Pending hardware run |
| Fresh bootstrap credentials after automatic reboot | Pending hardware run |
| Free/largest internal heap, PSRAM, and 24 KiB task high-water mark | Pending hardware run |
| Credential-free Serial log audit | Pending hardware run |

## Generators and regression fixtures

| Tool | Purpose |
| --- | --- |
| `tools/generate_browser_freesans_font.py` | Reproducibly generates `src/ui/fonts/BrowserFreeSans.h` from redistributable GNU FreeSans. Preserve `LICENSE.FreeFont`. |
| `tools/generate_legacy_video_fixtures.sh` | With FFmpeg 9.0.1, creates H.263/3GP and MPEG-4 Part 2/MP4 176×144, 10fps fixtures in `tests/video/fixtures/`. |
| `tools/png_to_icon.py` | Converts PNG artwork into project icon assets. |
| `tools/png_to_rgba565_icon.py` | Converts an RGBA PNG into flash-resident RGB565 colour and 8-bit alpha arrays for antialiased, full-colour UI icons. |
| `tests/media_cache_contract/` | Hardware-independent Arduino sketch covering cache paths, internal-format registration, signatures, and exact source-fingerprint matching. |
| `tests/assistant_dependency_probe/` | Compile-time-only ESP32 diagnostic and local TLS WebSocket fixture for Stage 0 codec, trust, header, echo, reconnect, heap, timing, and stack gates. |
| `tests/xiaozhi_compatibility_probe/` | Compile-time-only Stage 1 hosted bootstrap, binding, WSS hello, MCP-disabled/omitted, and automatic-reboot credential-refresh diagnostic. |

Regenerate a derived asset only through its corresponding tool, review the generated diff, and preserve source/license information. Do not hand-edit generated browser-font headers.

## Change checklist

- Keep `FileFormats`, decoder behavior, cache format documentation, and browser download behavior in sync when adding a media type.
- Treat cache header/version updates as persistent-format changes: deliberately invalidate incompatible old data and document the migration behavior.
- Preserve responsive touch: worker code may perform decoding or network/file I/O, but UI painting happens on the UI task and shared SD/touch SPI use stays granular.
- For browser throughput changes, record Browser Serial timings for cold/warm page loads and fixed/chunked HTTPS downloads. Tune the receive buffer before SD transaction size, use medians of three runs, then require ten successful cancel/retry cycles with touch polling before adopting a larger value.
- When adding a screen, follow [ui-kit.md](ui-kit.md) and [architecture.md](architecture.md), register it in the composition root, and give it only the controllers it needs.
- For USB Drive changes, verify the native connector does not enumerate during normal boot, transfer boot exposes an MSC-only complete card while UART logging remains available, Finder eject triggers a normal restart, and 512-byte raw transactions remain mutually exclusive with touch through `SpiBusLock`.
