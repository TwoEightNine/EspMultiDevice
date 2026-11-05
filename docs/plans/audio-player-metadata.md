# Plan: Audio Player track metadata and redesign

Status: phases 1–4 implemented; phase 5 not started. The contracts of record are the Audio section of [media.md](../media.md) and the Audio Player exception in [ui-kit.md](../ui-kit.md). `tests/audio_tags` passes on the host and the firmware builds; nothing has been run on a device yet. Differences from the proposal below:

- `AudioTags::Info` has no `coverKind`. Signatures belong to `FileFormats`, which needs Arduino, so phase 3 classifies the picture bytes there before decoding.
- Playback starts from `tick()` after the tags are read, for WAV too, rather than straight from `onEnter()`. Since phase 2 the file itself is also opened from the first `tick()`, after the screen is drawn.
- Phase 2 pulled two small pieces forward. WAV seeking (phase 4's WAV half) works, so the seek-bar drag path is exercised; MP3 shows its fill without a thumb until phase 4. ⏮ restarts the track, and Repeat All/One both restart it at the end without stopping the speaker; phase 5 gives them the folder queue. ⏭ is drawn inactive.
- The mute glyph is an × beside the speaker rather than a slash.
- Track info is a `drawPopupMenuFrame` sheet (y 96–371); any tap closes it and redraws the screen.
- Phase 3 moved `BitmapScale` to `src/media/` along with the new `BitmapDecode`, so the shared decoder does not depend on the apps folder. `BitmapDecode` takes the source cap and box in its options; `AppBitmapDecode` keeps the SDK 6 limits and maps the allocation error back to "App memory unavailable".
- The worker composes the whole tile, including the `#222` margins and antialiased rounded corners, so the UI pushes one 208×208 image with `pushImage`. DMA was not needed: the display is on its own parallel bus and the push happens once per track.
- Pictures smaller than 208×208 are centred at their own size rather than enlarged, because `BitmapScale` never upscales.
- The worker task starts with the first cover request and is deleted in `releaseResources()`. The underrun check with a large cover at 320 kbps is still a device test.
- Phase 4's table is the Arduino-free `Mp3SeekTable` (`src/media/`), host-tested in `tests/mp3_seek_table`. The `.empa` v2 header is 44 bytes: besides `tocInterval` and `tocCount` it stores the samples per MP3 frame (bytes 34–35, formerly padding), so a cache hit can map positions to frames and validate the entry count.
- The lookup restarts at the last entry strictly before the target frame, so there is always one frame to discard for the bit reservoir. The discard frames run inside `stream()`, which reports Busy until the target frame plays. While paused, they wait for the next play.
- A track without seek-table memory (PSRAM exhausted) still plays and restarts, and its `.empa` is not written, so it is scanned again on the next open.

Goal: the Audio Player shows the track's cover, title, artist, album, and year when the file carries them, and falls back to the file name when it does not. The screen moves to a conventional "now playing" layout: big artwork, a seek bar you can tap or drag, previous/next with auto-advance through the folder, and a volume slider in place of the full-screen 0–10 grid.

## Where things stand

- `AudioPlayerScreen` (`src/features/audio_player/`, ~1,000 lines) holds WAV parsing, MP3 analysis, the `.empa` cache, streaming, and drawing in one class.
- MP3: `beginMp3Analysis()` reads the ID3v2 header only to skip it (`_mp3DataOffset`), and checks for ID3v1 only to trim `_mp3AudioEnd`. Nothing reads tag contents.
- WAV: `parseWav()` walks every RIFF chunk but keeps only `fmt ` and `data`. It ignores `LIST`/`INFO` and `id3 `.
- UI: centred file name, a 48px `fillCircle` play button (pixelized, not kit), a "Volume: N / 10" bar that opens a 6-column level grid, an 8px square progress bar, and the error or "Loading MP3..." text at y=250. Below y≈270, 210px of the 480px screen are empty.
- No seeking, previous/next, or auto-advance. Files pushes a single `ScreenData::audioPath`.
- Every SD read happens on the UI task without `SpiBusLock`. That is safe today only because touch polling runs on the same task.
- Reusable pieces: `AppBitmapDecode` (in-memory PNG/baseline JPEG to a fitted RGB565 bitmap in PSRAM, JPEGDEC + PNGdec + `BitmapScale` box filter), `FilesScreen::naturalCompare`, and the `file-thumbs` worker pattern.

## Target layout (320 × 480 portrait)

```
y   0 ┌────────────────────────────────┐
      │ ✕  Now playing               ⋯ │  shared 48px app bar; ⋯ = Track info
 48   ├────────────────────────────────┤
 64   │         ┌──────────────┐        │
      │         │    cover     │        │  208 × 208, x 56–263
      │         │  or ♪ tile   │        │  placeholder: #222 smooth rounded tile + note glyph
272   │         └──────────────┘        │
288   │          Track title            │  Title face, white, 1 line, ellipsized
314   │          Artist                 │  Content face, light grey
336   │        Album · 1997             │  Compact face, grey; line omitted if both missing
360   │ ━━━━━━━━━━━●──────────────────  │  seek bar x 16–303; 4px track, 14px thumb
      │                                 │  hit target y 344–383, full width
386   │ 1:23                     -2:41  │  Compact; elapsed / remaining
412   │  ⟲     ⏮     ( ▶ )     ⏭     🔊  │  centre y 436
460   │                                 │  play: 64px white fill-only circle, black glyph
480   └────────────────────────────────┘  others: 48 × 48 cells at x centres 40/100/220/280
```

- **Title fallback:** the file name without its extension. **Artist fallback:** "Unknown artist" in grey. Then the screen never shows an empty slot, and untagged files still look intentional.
- **Errors and loading** take the artist line's place: red error text, or "Reading track…" with `UiKit::drawProgressBar` in indeterminate mode. The cover tile stays visible.
- **Volume popover:** the 🔊 cell opens a smooth rounded panel over the seek row (y 336–396). It holds a speaker glyph, a 0–10 slider, and the value. Tapping or dragging the slider sets the volume live. The value persists through `AudioSettings` on release (one NVS write per gesture). A tap outside the panel or 3 s without input closes it, and the seek row redraws. The speaker glyph shows a slash at volume 0.
- **Repeat cell (⟲):** cycles Off → All (folder) → One. It is cyan when active. The state is kept only for the screen visit; nothing persists.
- **Track info (⋯):** a `drawPopupMenuFrame` sheet with the file name, format, sample rate and channels, bitrate (MP3), and duration. It keeps the file name reachable now that the title replaces it.
- **Redraw policy:** draw the static parts (cover, text, transport) once per track. While playing, refresh only the seek bar and the two time labels, at the existing 200 ms cadence, by repainting just those rectangles. Push the cover once, when the worker delivers it.

## Phase 1: Tag parsing (Arduino-free, host-tested)

New `src/media/AudioTags.{h,cpp}` with no Arduino or SD dependency, so `tests/audio_tags/` can run it on the host the way `tests/bitmap_scale` does. It reads through a small `read(offset, buf, len)` callback and yields:

```cpp
struct AudioTagInfo {
  char title[128], artist[128], album[128];  // UTF-8, trimmed, control chars removed
  uint16_t year;                             // 0 = unknown
  uint32_t coverOffset, coverLength;         // 0 = none; absolute file offsets
  uint8_t coverType;                         // ID3 picture type; 3 = front cover
};
```

- **ID3v2.2/2.3/2.4:**
  - Frames: `TIT2`/`TT2`, `TPE1`/`TP1` (falling back to `TPE2`/`TP2`), `TALB`/`TAL`, `TDRC`/`TYER`/`TYE`, and `APIC`/`PIC`.
  - Header handling: skip the extended header. Frame sizes are synch-safe in v2.4, plain in v2.3. Honour tag-level (v2.3) and frame-level (v2.4) unsynchronisation for text frames.
  - Encodings: 0 Latin-1, 1 UTF-16 with BOM, 2 UTF-16BE, 3 UTF-8. Convert all to UTF-8 and bound each field at 127 bytes.
  - Year: the first four digits of `TDRC`/`TYER`, accepted from 1000 to 2999.
  - Cover: prefer picture type 3 (front cover), else the first picture. Record its payload offset and length; the parser never reads the image bytes. An unsynchronised or compressed `APIC` is treated as no cover.
- **ID3v1/v1.1:** fills only the fields ID3v2 left empty. Its fixed 30-byte fields are space or NUL padded.
- **Cyrillic heuristic (decision):** Latin-1 text (ID3v1, or ID3v2 encoding 0) whose high bytes all fall in 0xC0–0xFF, 0xA8, or 0xB8 decodes as Windows-1251. Russian collections commonly store cp1251 there, and `UiText` renders Cyrillic. Text with any other high byte stays Latin-1.
- **WAV:**
  - `parseWav()` already walks the chunks. It also records the `LIST`/`INFO` offset (`INAM` title, `IART` artist, `IPRD` album, `ICRD` year) and any `id3 `/`ID3 ` chunk.
  - `AudioTags` parses both. The embedded ID3 takes precedence and can supply the cover.
- **Bounds:** every frame or chunk size is checked against the tag or file end before use, and parsing stops at the first inconsistent size. At most 256 frames are visited, and the parser skips each frame body with a seek rather than reading it, except text frames, of which it reads at most 256 bytes. A 2 MB tag holding a large picture therefore costs only header reads.
- **Scheduling:** `AudioPlayerScreen` runs the parser as the first incremental step of loading (8 frames per `tick()`), before MP3 analysis. Text appears within a few ticks, while the MP3 scan continues with its existing "Reading track…" state.
- **No text cache:** the tag is re-parsed on every open, which is a handful of small reads, so no new persistent format is needed for text.

## Phase 2: Layout and UiKit components

Add these to `UiKit`, with signatures documented in `docs/ui-kit.md`:

| Component | Notes |
| --- | --- |
| `drawSeekBar(tft, x, y, width, fraction, thumb, fg, track, bg)` / `updateSeekBar` | Smooth 4px pill track, cyan fill, 14px white thumb. `updateSeekBar` repaints only the changed span plus the old and new thumb. It is also the volume slider. |
| `drawSkipIcon(tft, cx, cy, forward, fg, bg)` | 24px antialiased previous/next. |
| `drawVolumeIcon(tft, cx, cy, muted, fg, bg)` | 24px speaker with waves or slash. |
| `drawRepeatIcon(tft, cx, cy, one, fg, bg)` | 24px loop arrows, with a "1" badge for repeat-one. |
| `drawMusicNoteIcon(tft, cx, cy, size, fg, bg)` | Cover placeholder glyph (smooth primitives, scalable). |

- The large play control reuses `drawPlaybackIcon` scaled to the 64px circle. `docs/ui-kit.md` records it as the player's screen-specific exception: a 64px white fill-only circle with a black glyph, and the whole 64 × 64 cell as the target.
- The seek bar and volume slider need drag, but the main loop forwards only touch edges. Like Home's strip, the screen reads `TouchController`'s held point in `tick()` while a drag that began on the slider is active. Seek commits on release, and the thumb plus a time preview track the finger meanwhile.
- `drawVolumePicker`, `volumePickerLevelAt`, `drawVolumeButton`, and the `fillCircle` play button are removed.

## Phase 3: Cover art (worker-backed)

- **Shared decoder:** move the body of `AppBitmapDecode` to `src/media/BitmapDecode.{h,cpp}`, with the source-size cap as a parameter. `AppBitmapDecode` becomes a thin wrapper that keeps the SDK 6 limits (128 KiB source, 320 × 432 box), so app behaviour does not change.
- **`AudioCoverWorker`** (`src/features/audio_player/`): screen-scoped and pinned to the core opposite the UI loop, priority 1, like `file-thumbs`.
  - Input: path, access mode, `coverOffset`/`coverLength`/`coverKind`, and a generation number.
  - Reads the picture into a PSRAM buffer in 8 KiB chunks, holding `SpiBusLock` for one chunk at a time. Pictures larger than 1 MiB are skipped.
  - Decodes and fits the picture to 208 × 208 (aspect kept, centred on the `#222` tile colour), then frees the source buffer at once.
  - Publishes the result generation-tagged. The UI task pushes the pixels into the cover tile (DMA when available, else `pushImage`). Track change, SD removal, and `onExit()` cancel the job and wait for the acknowledgement before freeing anything.
  - Unsupported variants (progressive JPEG, interlaced PNG, WebP covers) or a PSRAM shortage leave the placeholder in place. The serial log names the reason.
- **SPI correctness (required by this phase):** once a worker touches SD while the player streams, the player's own source reads and seeks must also take `SpiBusLock` per transaction. Those are `refillMp3Input`, the WAV `read`, `seekTo`, and the analysis header reads.
  - Use a short timeout. On a miss, skip this `tick()` and retry; the ~200 ms I2S DMA queue absorbs it.
  - Rule out an underrun on a real device with a large-cover MP3 at 320 kbps, and do so before Phase 3 lands.
- **No cover sidecar at first:** decoding a typical 500–600px JPEG at a 1/2 reduction on the worker should take well under 300 ms and never blocks the UI. If a measurement shows otherwise, add an appended `FileFormats` internal format `.emcv` (fingerprinted 208 × 208 RGB565, `.tmp` → rename through `MediaCache`). Cleanup and secret moves then cover it automatically.

## Phase 4: Seeking

- **WAV:** set `_framePosition` and seek to `_dataOffset + frame × blockAlign`. No new data is needed.
- **MP3:** while the existing analysis pass walks every frame, record the byte offset of every Nth frame in a PSRAM table. N starts at 64 (≈1.7 s at 44.1 kHz) and doubles whenever the table would pass 4,096 entries.
  - A seek resets minimp3 at the nearest entry at or before the target, then decodes and discards whole frames up to the target, at most 8 frames per `tick()`.
  - The first decoded frame after a reset is discarded, to avoid the bit-reservoir glitch.
- **`.empa` version 2:** append `tocInterval` and `tocCount` to the header, followed by `tocCount` little-endian u32 offsets (at most 16 KiB).
  - Version-1 files are rejected and rebuilt on next open, per the cache rules.
  - Validation: entries strictly increasing and inside `[dataOffset, audioEnd)`, and the first one equal to `dataOffset`.
  - Update the `.empa` section of `docs/media.md`.
- **Until analysis finishes,** the seek bar is disabled: grey track, no thumb.

## Phase 5: Folder queue, previous/next, auto-advance

- **Queue scope:** the player builds its own queue from the source's folder, so Files, the Assistant, and any future caller keep pushing the same single-path payload, and `ScreenData` gains no list.
- **Queue build:**
  - The scan reads one directory entry per `tick()` under `SpiBusLock`. It keeps names whose `FileFormats` class is openable audio, skipping hidden entries and `.cache`.
  - It holds at most 512 names in PSRAM, sorted by the natural order Files uses. Extract `FilesScreen::naturalCompare` into `src/core/NaturalSort.h` so both use one implementation.
  - The current track's index is found by name.
  - In a secret folder the scan goes through `SecretStorage`'s listing with the same access mode as the source. If that cannot list without re-auth, ⏮/⏭ stay inactive (grey icon, no fill) in secret mode.
- **Behaviour:**
  - ⏮ restarts the track when more than 3 s have played, otherwise goes to the previous track. ⏭ goes to the next track.
  - At the end of a track: Repeat One restarts it; otherwise the next track plays. At the folder's end, Repeat All wraps to the first track, and Off stops on Finished.
- **Track change:** happens in place: stop the speaker, cancel the cover job, `reset()`, `loadSource(next)`, redraw. There is no `Navigator::replace`, so the screen and popover state stay put and nothing flickers through Files.
- **Failures:** a track that fails to open shows its error for 2 s, then auto-advance skips it. A run of 3 consecutive failures stops playback.

## Code shape

`AudioPlayerScreen.cpp` would pass 1,500 lines with all this, so phases 1–5 also split it:

- `AudioTags` (phase 1, `src/media/`): tag parsing.
- `AudioSource` (`src/features/audio_player/`): WAV/MP3 open, analysis, `.empa`, TOC, seek, and PCM streaming. This moves out of the screen unchanged in phase 2, before the new layout lands, so the redesign diff stays reviewable.
- `AudioCoverWorker`: cover worker.
- `AudioPlayerScreen`: layout, touch, queue, and state machine only.

## Verification

- **Host:** `tests/audio_tags/`, with fixtures written by a new dependency-free `tools/generate_audio_tag_fixtures.py`, covers:
  - ID3v2.2, 2.3, and 2.4
  - UTF-16 with and without a BOM
  - unsynchronised text
  - JPEG and PNG `APIC`, with a non-front picture listed first
  - an oversized frame size, a truncated tag, and an ID3v1-only file
  - cp1251 in ID3v1
  - WAV `LIST`/`INFO` and a WAV `id3 ` chunk

  It also covers `BitmapScale` and the `.empa` v2 validation.
- **Build:** `arduino-cli compile` with `sketch.yaml` after each phase.
- **Device:**
  - a tagged MP3 with a 600 × 600 baseline JPEG cover
  - a progressive-JPEG cover (placeholder expected)
  - an untagged MP3 (file-name fallback)
  - a WAV with INFO
  - a secret-folder MP3
  - SD removal mid-track and during cover decode
  - drag seek while playing and paused
  - a stale v1 `.empa` (rebuilt)
  - auto-advance across a folder with one broken file
  - no underruns with the cover worker active (serial log)
- **Docs:**
  - `docs/media.md`: the Audio section, and `.empa` v2.
  - `docs/ui-kit.md`: the new components, and the 64px play exception.
  - `docs/architecture.md`: the cover worker, and the player's SD reads now taking `SpiBusLock`.
  - `README.md` and `CHANGELOG.md`: a one-line summary.

## Out of scope

- Files list thumbnails from album art.
- AAC/M4A, FLAC, or Ogg audio.
- Persisting repeat mode or a resume position.
- Background playback after leaving the screen.
- Lyrics.
