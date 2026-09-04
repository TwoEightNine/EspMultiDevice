# Development, dependencies, and third-party code

## Build inputs

Open `esp_media.ino` as an Arduino sketch. Arduino recursively compiles `src/`; `build_opt.h` supplies `PNG_MAX_BUFFERED_PIXELS=32832` and include paths for the bundled Helix, lib264, and FFmpeg subsets.

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

Do not casually upgrade, reconfigure, or copy a dependency into the Arduino libraries directory. A change can alter compilation boundaries, binary size, codec behavior, and license obligations. Keep portable generic-C video paths; architecture-specific SIMD sources are deliberately excluded.

## Generators and regression fixtures

| Tool | Purpose |
| --- | --- |
| `tools/generate_browser_freesans_font.py` | Reproducibly generates `src/ui/fonts/BrowserFreeSans.h` from redistributable GNU FreeSans. Preserve `LICENSE.FreeFont`. |
| `tools/generate_legacy_video_fixtures.sh` | With FFmpeg 9.0.1, creates H.263/3GP and MPEG-4 Part 2/MP4 176×144, 10fps fixtures in `tests/video/fixtures/`. |
| `tools/png_to_icon.py` | Converts PNG artwork into project icon assets. |

Regenerate a derived asset only through its corresponding tool, review the generated diff, and preserve source/license information. Do not hand-edit generated browser-font headers.

## Change checklist

- Keep `FileFormats`, decoder behavior, cache format documentation, and browser download behavior in sync when adding a media type.
- Treat cache header/version updates as persistent-format changes: deliberately invalidate incompatible old data and document the migration behavior.
- Preserve responsive touch: worker code may perform decoding or network/file I/O, but UI painting happens on the UI task and shared SD/touch SPI use stays granular.
- When adding a screen, follow [ui-kit.md](ui-kit.md) and [architecture.md](architecture.md), register it in the composition root, and give it only the controllers it needs.
