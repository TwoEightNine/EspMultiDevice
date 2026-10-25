# Animated-image fixtures

`tools/generate_animation_fixtures.py` writes these fixtures to `fixtures/`
with Pillow 12.3.0 (libwebp 1.6.0). GIFs come from the script's own GIF89a
encoder, so their bytes do not depend on Pillow; WebP bytes do. The script
decodes every fixture again with Pillow and fails on a frame-count, loop, or
lossless pixel mismatch.

```sh
python3 tools/generate_animation_fixtures.py
```

## What a frame shows

- A three-digit frame number. Skipped numbers are dropped frames.
- A yellow marker sweeping along the bottom strip, one step per frame.
- A square near the top-right that cycles red → green → blue every frame.
- A grey ramp across the top: 32 steps in GIFs, a smooth gradient in WebP.
  RGB444 shows 16 bands; RGB565 shows 32.
- A 1 px white border. A missing edge means the frame is cropped or offset.

## Fixtures

| File | Expected result |
| --- | --- |
| `gif_160x120_100ms.gif` | 30 frames at 10 fps, centred at native size (never upscaled). |
| `gif_320x240_100ms.gif` | 30 frames at 10 fps, filling the 320 × 240 viewport. The main smoothness check. |
| `gif_320x240_40ms.gif` | 50 frames authored at 25 fps. The cached player clamped it to 10 fps; streaming plays at 25 fps when decode and presentation keep up, and otherwise skips numbers. |
| `gif_320x240_0ms.gif` | Zero delays. Plays at 10 fps (delays of 10 ms or less become 100 ms). |
| `gif_317x237_100ms.gif` | Odd width and height. Rows must not skew or shift. |
| `gif_640x480_100ms.gif` | 20 frames downscaled to 320 × 240. |
| `gif_1000x1000_100ms.gif` | 12 frames downscaled to 240 × 240. Stresses PSRAM and decode speed. |
| `gif_320x240_textured_100ms.gif` | 20 frames over scrolling noise. Worst-case LZW decode, similar to dithered photos. |
| `gif_no_loop.gif` | No NETSCAPE extension: plays frames 0–9 once, then holds frame 9. |
| `gif_loop2.gif` | Loop count 2: three passes in total, then holds frame 9. |
| `gif_single_frame.gif` | One frame (frame 000) shown still. |
| `gif_truncated.gif` | The 320 × 240 loop cut mid-frame with no trailer. Streaming loops the frames that decoded, and serial logs `[Anim] GIF stream ended early`. |
| `gif_disposal_alpha.gif` | A checkerboard, then 64 × 64 disks with transparent corners. Red (disposal 1) disks accumulate. Green (disposal 2) disks leave black holes in the board. Blue (disposal 3) disks vanish and the board shows through again. The checkerboard must show through the disk corners. Magenta anywhere means transparency or disposal is wrong. |
| `webp_anim_lossy_320x240.webp` | Lossy, 30 frames at 10 fps. |
| `webp_anim_lossless_320x240.webp` | Lossless, 30 frames at 10 fps. |
| `webp_anim_640x480_lossy.webp` | Lossy, 20 frames downscaled to 320 × 240. |
| `webp_anim_textured_320x240_lossy.webp` | Lossy, 20 frames over scrolling noise. Worst-case WebP decode. |
| `webp_anim_alpha_blend.webp` | Left half transparent and right half an opaque board. A 50 %-alpha white square moves across. On the device's black canvas, the left half is black and the square is mid grey there. |
| `webp_static.webp` | A simple (`VP8 `) 480 × 200 still. Uses the `.em16` cache and offers crop zoom. |
| `webp_static_alpha.webp` | An extended (`VP8X`, alpha, no animation) still. Stays on the static path. |

## Baseline procedure

Use this to compare the cached player with the streaming replacement.

1. Build and flash with `tools/build_esp32s3.sh`, then open the serial monitor at 115200 baud.
2. Copy `fixtures/` to the SD card. For a clean first-open measurement, delete the folder's `.cache/*.emga` files on a computer.
3. Open each fixture from Files twice: once to convert, then once again as a cache hit. Watch about 30 s of each.
4. Record from serial:
   - `[Anim] converted N frame(s) in T ms` (first open only)
   - `[Anim] first frame T ms after open` (both opens)
   - the last `[Anim] pres=… drop=… underruns=… psram=free/largest` line
5. Record by eye: skipped numbers, stalls, ramp bands, and disposal or alpha errors.
6. Record the `.emga` size from the card.

| Fixture | Convert ms | First frame (convert) ms | First frame (cache hit) ms | pres / drop / underruns after ~30 s | PSRAM free / largest | `.emga` bytes | Visual notes |
| --- | ---: | ---: | ---: | --- | --- | ---: | --- |
| `gif_320x240_100ms.gif` | | | | | | | |

## Phase 1 checks: GIF streaming

GIFs now stream from the source, so the baseline's conversion and cache-hit
columns no longer apply to them. Serial reports:

- `[Anim] first frame T ms after open`
- every 2 s, `[Anim] dec=… pres=… drop=… decMs=avg/max scaleMs=avg psram=free/largest`
- `[Anim] worker stack free N bytes` when the viewer closes

Animated WebP still uses the cached path until phase 2.

1. Open each GIF twice. Both opens should report a similar first-frame time,
   far below the baseline's first-open conversion.
2. Watch `gif_160x120_100ms.gif` and `gif_320x240_100ms.gif` for 30 s. No frame
   number is skipped and `drop=0`.
3. Step `gif_disposal_alpha.gif` against a desktop browser. Red disks
   accumulate, green leave black holes, blue vanish, and nothing is magenta.
4. `gif_no_loop.gif` stops on frame 9 after one pass. `gif_loop2.gif` stops
   after three passes. After either stops, tapping still toggles the bar and
   frame 9 repaints.
5. Tap the centre about ten times during playback. The bar toggles cleanly, and
   in informative mode it never flickers as frames change.
6. Edge-tap quickly between two animated GIFs and a still image about ten times.
   Nothing crashes or shows a stale frame, each close logs the worker stack, and
   `psram=` free returns to the same value on each open.
7. Close `gif_1000x1000_100ms.gif` while "Loading animation" is showing. The
   viewer exits at once. Opened normally, it plays (possibly dropping frames) or
   shows a PSRAM error with figures; it never hangs.
8. Remove the SD card during playback. The error panel appears without a crash.
   Reinsert the card and reopen; playback works.
9. Move a GIF into secret storage, unlock, and open it. It plays.
10. On a computer, confirm that no new `.cache/*.emga` appears for any GIF.
11. In the Lua Images example app (`examples/apps/images`), open a GIF. It
    plays, and `esp.image.status()` goes from `loading` to `ready` with
    `animated=true`.
12. `gif_truncated.gif` loops its valid frames. `gif_single_frame.gif` shows
    frame 000, and `dec=` stays at 1.
13. Record `decMs` for `gif_320x240_textured_100ms.gif`, `gif_640x480_100ms.gif`,
    and `gif_1000x1000_100ms.gif`. These numbers decide whether phase 6 (PSRAM
    replay) is worth doing.
