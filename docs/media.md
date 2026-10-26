# Media support and cache formats

## Central format registry

`src/media/FileFormats.h` is the sole source of truth for file classification. It resolves extensions, HTTP content types, and file signatures for BMP, PNG, JPEG, GIF, WebP, WAV, MP3, MP4/M4V, and 3GP/3GPP, plus the local text extensions `.txt`, `.json`, `.lua`, and `.config`. Browser and Files must use it consistently. Text formats are openable from Files but are not Browser-downloadable media.

`.eth1`, `.em16`, `.em16z`, `.emga`, and `.empa` are internal cache files; `.emga` is a legacy format that nothing writes any more (`FileFormats::isLegacyCache`). They keep their image/audio grouping but are never openable or downloadable source media. Their extensions and internal classification live in `FileFormats`; new enum values must be appended because the ETH1 header persists the numeric source format.

Assistant image and video filename searches use this registry and the same
normal-SD privacy filter as audio search. They return at most five opaque
selections, revalidate the selected source before launch, and hand off to the
existing Image Viewer or Video Player after the Assistant session stops.
Internal cache files are never selections.

## Text viewer

Files opens `.txt`, `.json`, `.lua`, and `.config` sources in the read-only
Text Viewer. Extension matching is case-insensitive; extensionless files and
similar suffixes such as `.conf` remain unknown. All four formats use the same
plain-text presentation without syntax highlighting, JSON validation, or
reformatting.

The viewer uses the shared 48px app bar and smooth `UiText` Content face. It
wraps at whitespace when possible, hard-wraps uninterrupted tokens, preserves
explicit blank lines, expands tabs to four spaces, and pages through the
document with shared footer arrows. The footer reports `Page N` and byte
progress rather than pre-scanning the complete file for a total page count.
Page navigation repaints the document viewport and changed footer state while
leaving the app bar in place.

Input is UTF-8 or its ASCII subset. An optional UTF-8 BOM is skipped, CRLF and
CR are normalized as line endings, malformed sequences and other control bytes
render as replacement question marks, and the normal Latin/Cyrillic font
fallback applies to unsupported glyphs. UTF-16 BOMs produce an explicit error.

Only the visible page is retained. The screen reads at most 512 source bytes
per UI tick under `SpiBusLock`, keeps 64 recent page offsets, and incrementally
rescans from a known boundary when older history is needed. It creates no SD
cache. The same `StorageFile` path supports normal and protected files; SD
removal closes the source, normal files restart from page one after remount,
and protected files require a new unlock.

## Image viewer

The viewer supports BMP, static non-interlaced PNG, baseline JPEG, simple/extended WebP, GIF87a/GIF89a, and animated WebP. Animated PNG and progressive JPEG are unsupported. Source images render aspect-fit on the 480 × 320 display.

Images open in informative mode with an opaque 48px top bar showing the source filename. Tapping the central image area toggles informative and full-screen modes; full screen hides the bar. The left and right 48px edges move through adjacent images, except that the informative bar owns its close target and, when available, its top-right zoom target. Eligible still images whose aspect ratio differs from the 480 × 320 screen ratio by more than 10% expose a centered crop-to-fill zoom toggle. Crop state persists while switching modes. Edge navigation preserves full-screen mode but resets zoom; every newly opened image starts aspect-fit in informative mode. Animated images do not expose crop zoom.

The decode, scaling, and cache code lives in `ImageView`, and animation decoding in its `AnimationStreamWorker` (both in `src/features/image_viewer/`), which the Image Viewer screen and the Lua app host share; the screen keeps only its bar, touch targets, and neighbour navigation. SDK 5 Lua apps show images through the same pipeline (`esp.image`, see [external-apps.md](external-apps.md)), so they use and produce the same `.em16` and `.em16z` sidecars. Because a Lua worker may use the card while an image decodes for it, every source read, seek, and open in `ImageView` takes `SpiBusLock` for that one transaction (waiting up to 250 ms), as `MediaCache` already does for sidecars.

From Files, tapping the left or right 48px edge moves to the previous or next supported image from the naturally sorted folder listing; navigation wraps. The Image Viewer itself has no navigation arrows.

Animated images fit a 320 × 240 photo viewport. Larger images are downscaled; smaller images are never upscaled. Animation support includes transparency, disposal/blending, and loop metadata. Insufficient PSRAM produces an error instead of synchronous SD fallback.

Animated GIF and WebP stream directly from their source and never read or write an `.emga` sidecar; existing sidecars remain orphaned cache files until Files deletes or renames their source. `ImageView` routes a WebP to streaming when its first chunk is `VP8X` with the animation flag set, reading the canvas size from that header. Simple (`VP8 `/`VP8L`) and non-animated extended WebP stay on the cached still path.
- **Worker:** `AnimationStreamWorker` runs on the core the Arduino loop does not use and owns the source file and the decoder while it runs. It scales each frame, nearest-neighbour with precomputed row and column maps, into one of three packed RGB565 PSRAM slots sized to the draw rectangle.
  - **GIF** decodes into AnimatedGIF's cooked PSRAM canvas, which holds five bytes per source pixel plus three per column. Each GIF stream allocates its own `AnimatedGIF` state (about 25 KB of LZW tables and file/line buffers) in internal RAM when at least 32 KB of internal RAM would stay free, and otherwise in PSRAM. Serial logs `[Anim] GIF decoder state N bytes in internal|PSRAM RAM (internal free F, largest L, reserve R)`.
  - AnimatedGIF and libwebp build at `-O2` through their `src/vendor_shims/` wrappers, like lib264, instead of the sketch-wide `-Os`.
  - **WebP** first loads the whole source into PSRAM through an 8 KiB internal bounce buffer, one bounded SD transaction per chunk. It then decodes with libwebp's `WebPAnimDecoder` in premultiplied `MODE_rgbA`, so transparency composes over black. libwebp keeps current and previous RGBA canvases (eight bytes per canvas pixel) beside the source.
  - Before starting, the worker checks free and largest-block PSRAM against these figures. It refuses with the required and available sizes rather than failing part-way.
- **SD access:** every source open, read, and seek holds `SpiBusLock` for that one transaction. A stop request abandons a pending lock wait, so closing or navigating away waits for at most the current frame's decode.
- **Timing:** the UI starts the presentation timeline when it shows the first frame. Each frame becomes due at the sum of the preceding delays, and the UI shows frames strictly in order.
  - Delays of 10 ms or less, including frames without a delay, play at 100 ms, as in browsers. Other delays are honoured, with no further minimum.
  - Every decoded frame is shown; nothing is skipped. Unlike video there is no audio clock, and each frame must be decoded anyway because later frames build on its canvas, so skipping would save only the cheap scale and transfer.
  - A frame ready more than 20 ms after its due time is shown at once and restarts the timeline from itself. When the decoder is slower than the authored delays, playback therefore slows to the decoder's pace (slow and smooth) and never fast-forwards to catch up. Smaller lateness, such as a DMA transfer still in flight, is absorbed without shifting the timeline.
- **Looping:** each loop pass starts from a cleared canvas.
  - **GIF:** a file without a NETSCAPE extension plays once, a loop count of zero repeats forever, and N repeats N more times.
  - **WebP:** the loop count is the total number of passes, and zero repeats forever.
  - The final frame stays on screen, and the worker releases the source and decoder.
- **Resident replay:** a looping animation (anything but a single pass) records its first pass, giving each frame its own packed PSRAM slot. The slot table is sized at start from free PSRAM minus the decoder, capped at 512 frames. A WebP whose frame count already exceeds it never records.
  - While recording, the worker allocates a new slot per frame. Before each allocation it checks that the frame still leaves 512 KB of PSRAM free.
  - When a frame no longer fits, recording is abandoned: shown frames are freed, playback continues streaming every loop, and surplus buffers return to PSRAM as they are shown. Serial logs `[Anim] first pass does not fit in PSRAM; streaming every loop`.
  - When the first pass completes in memory, buffers that hold no first-pass frame are freed, and the worker releases the source, decoder, canvas, and scaler maps and stays idle. Serial logs `[Anim] replaying N resident frame(s) (K KB); decoder released`.
  - Later passes show the resident frames at their authored delays on the same timeline, honouring the loop count. The first pass still plays at decode speed when decoding is slow.
- **Damaged files:** a single-frame animation is decoded once. A truncated or damaged file loops the frames that decoded; it fails only if a pass cannot decode its first frame.
- **Display:** the first frame replaces the black canvas directly. When it takes longer than 150 ms, a "Loading animation" label appears meanwhile, with the WebP source-load percentage or, for GIF, an indeterminate progress bar. Each frame goes to the panel as one asynchronous LCD_CAM DMA transfer from its 64-byte-aligned slot (`DisplayController::beginDmaImage()`), so a whole frame appears at once and the UI loop keeps polling touch meanwhile.
  - The UI takes no new frame until the transfer completes, and it finishes any transfer before the worker may free slots.
  - Stopping the stream releases the LCD_CAM bus, as the video player does.
  - In informative mode, rows under the 48px app bar are not sent, so the bar does not flicker. The clipped transfer starts at the first visible row.
  - When that row's offset is not 64-byte aligned (a width that is not a multiple of four), or DMA is unavailable, the frame falls back to 48-row TFT_eSPI bands, one band per loop pass. Serial then logs `[Anim] DMA unavailable for this frame; using TFT_eSPI bands` once per open.
- **Serial log:**
  - the time from open to the first frame
  - after the first pass, `[Anim] pass 1: N frame(s), T ms as authored, canvas WxH, frame B bytes`
  - every 2 s, `[Anim] stream|record|replay slots=… dec=… pres=… late=… lagMs=… decMs=avg/max scaleMs=avg psram=free/largest`, where `slots` counts allocated frame buffers, `late` counts timeline restarts, and `lagMs` totals the slowdown
  - the worker's free stack when it stops

Use `tools/generate_animation_fixtures.py` to generate the GIF/WebP fixtures under `tests/animation/fixtures/`; their expected results and the measurement procedure are in `tests/animation/README.md`.

Visible image files may receive 32×32 Files thumbnails. They are worker-decoded, retained for the Files visit, and persist as validated `.cache/*.eth1` sidecars. Unsupported variants, oversized WebP input, and memory failures retain a normal file icon.

## Assistant artwork

Assistant treats `/.system/ai_avatars/background.png` as private screen
artwork, not user-openable media. It must be a non-interlaced 320×432 PNG and
is decoded by the screen-scoped avatar worker into PSRAM before being pushed
one-for-one at display y=48. Its bubble artwork is restored before each caption
redraw. Source alpha is flattened against black during decode. A missing,
invalid, or wrong-sized file leaves a black background; there is no scaling,
cache sidecar, or synchronous UI-task SD decode.

## Lua app artwork

Home treats `/Apps/<id>/logo.png` as package artwork rather than user-openable
media. The app-catalog worker accepts only non-interlaced 64×64 PNGs that carry
alpha, validates the PNG CRC, and retains straight RGB565 plus 8-bit alpha in
PSRAM. The UI composites the pixels over the package's RGB565 circle color.
There is no scaling or cache sidecar; invalid artwork uses the generic Apps
glyph and does not invalidate an otherwise runnable package.

## Audio and recording

The recorder writes 16 kHz mono PCM WAV from the INMP441. The Audio Player streams mono or stereo integer PCM WAV and local MP3. Global volume is persisted through `AudioSettings` and ranges from 0 to 10 (default 5).

MP3 playback uses a sidecar cache at `<source-directory>/.cache/<file>.empa`. It stores validated compressed-audio bounds, format, and decoded sample count so later playback can skip a full MP3 scan while still decoding the original stream. It is valid only when source size and last-modified time match.

## Video

The physically inverted display module uses TFT/touch rotation 2 for the normal upright portrait presentation and rotation 3 for the existing counter-clockwise landscape presentation. Both orientations retain the same logical viewports and controls described below.

The Video Player opens ordinary local MP4/3GP ISO-BMFF files with H.264 Baseline/Main/High, H.263, or MPEG-4 Part 2 video at any source dimensions the decoder can safely configure and PSRAM can hold. It also opens complete, seekable local fragmented MP4 (fMP4) files when their video is H.264 and their optional audio is AAC-LC. The fMP4 reader accepts `mvex`/`trex` defaults and standard `moof`/`traf`/`tfhd`/`tfdt`/`trun` sample layouts, retaining only a presentation-time list and the active per-track fragment run in PSRAM; it does not support seeking, live/growing files, manifests, encryption, changing sample descriptions, H.263/MPEG-4 Part 2 fragments, or non-AAC fMP4 audio. H.264 first tries equal power-of-two reference/reorder reservations: 16/16, 8/8, 4/4, 2/2, 1/1, then 1/0. It reads the source SPS's mandatory reference-frame count; if the next smaller equal pool would violate that requirement, it instead retries the current reference count with lower reorder reserves before rejecting the file. This retains the references needed to decode a valid stream while spending less PSRAM on display reordering. Source YUV remains decoder-owned in PSRAM and is aspect-downscaled to the 320 × 480 display. The BT.601 nearest-neighbour converter (`VideoFrameScaler`, built at `-O2`) uses branch-free RGB565 saturation tables and a precomputed source-column map, both in internal RAM when available; native-size frames convert in 2 × 2 blocks that share one chroma sample, scaled frames reuse chroma across adjacent columns, and upscaled rows that repeat a source row are copied. Its output is bit-identical to the direct per-pixel formula; there is no source-resolution or minimum-FPS policy limit. The worker keeps two presentation frames only, so a fast decoder cannot run ahead of the audio clock. The UI sends each presented frame to the panel with an asynchronous LCD_CAM DMA transfer (about 15 ms for a full 320 × 480 frame at 20 MHz, compared with a CPU-bound TFT_eSPI GPIO write) and returns the slot to the worker when the transfer completes. H.264 always decodes its complete reference chain, but when an output is more than 250 ms late the worker skips only its RGB conversion and UI publication; this keeps later frames near real time without corrupting inter-frame references. Compatible AAC-LC, MP3, or AMR-NB (`samr`, 8 kHz mono) audio remains the real-time playback clock. Invalid dimensions, checked-allocation overflow, decoder limits, or PSRAM exhaustion produce a resource error with the requested geometry and free/largest PSRAM. Decoders enqueue mono PCM into a 16,384-frame PSRAM ring; startup requires 280 ms, normal refill begins below 220 ms and targets 300 ms. The 16-descriptor I2S DMA queue chooses its frames per descriptor from the configured rate, keeping about 200 ms of output at 8 kHz through 44.1 kHz instead of introducing an AMR-specific one-second queue. I2S writes remain non-blocking and individually capped at 256 frames, while a short pre-decode burst fills the DMA queue; AAC refill permits up to eight packets when needed to restore the audio reserve. A full I2S queue is backpressure, not a codec failure. The clock is active wall time clamped by submitted PCM duration, so an audio underrun also stops video progress.

Video streams directly from its source and never reads, writes, or removes an `.emga` sidecar. Existing video sidecars remain harmless orphaned cache files until normal Files cleanup. `VideoH264` wraps pinned lib264; `VideoLegacy` exposes H.263 and MPEG-4 Part 2 through the same planar I420 callback contract. Both decoder paths need PSRAM and configure from the source geometry after checked arithmetic. The second-core worker owns the source, demuxer, decoders, display-sized frame buffers, PCM ring, and I2S operations. Audio drain, up to four compressed audio decodes, and one available video sample are interleaved in bounded scheduling quanta. Video and audio samples each read through their own direct `StorageFile` view and 16 KiB sector-aligned PSRAM window, so each refill is one multi-block card read and neither stream moves the other's FatFs position backwards; without a free descriptor a stream falls back to the shared source file. Each refill or larger-sample 16 KiB chunk holds the shared SD/touch SPI lock only for one bounded transaction.

Videos open directly into a chrome-free 320 × 480 portrait full-screen view and start automatically once two decoded video frames and the audio minimum are buffered. Full screen has no title, status text, progress, or playback control; tapping it requests embedded mode and pauses playback after the first replacement frame is ready. Portrait embedded mode reserves the 48px app bar, titled with the source filename, and a 48px bottom control strip, leaving a 320 × 384 viewport. Its bottom-left 48 × 48px fullscreen control changes to counter-clockwise landscape (TFT/touch rotation 3). Landscape embedded mode has no app bar or title; it uses a 480 × 320 viewport and overlays its play/pause, progress, and bottom-left fullscreen-exit control over the video. Landscape full screen is chrome-free; tapping it returns to landscape embedded mode, where the orientation-exit control is available. The 40 × 40px play/pause control is centered on the aspect-fit active video frame, while taps elsewhere in the video canvas return to full screen. The control's underlying video pixels are retained and restored before leaving embedded mode, so a paused full-screen view never shows a stale play icon. Resuming through the embedded control automatically returns to full screen after two seconds of playback. Viewports are centered on the physical display. Mode and orientation changes safely replace the worker's packed frame slots without restarting source, decoder, audio, or playback timing; the newly exposed canvas is black until a replacement frame arrives. Any terminal opening or playback error, including an unsupported format, restores portrait embedded mode so its app-bar close target is available. Frames remain aspect-preserving; unused space is black, so sources are never cropped or stretched. H.264 output that is more than 250 ms late remains decoded for reference integrity but is not converted or published; a subsequent non-stale output replaces it. Use `tools/generate_legacy_video_fixtures.sh` and FFmpeg 9.0.1 to generate reproducible 320 × 480, 30fps H.263, MPEG-4 Part 2, and H.264/AAC fixtures under `tests/video/fixtures/`.

The adaptive full-frame policy above supersedes the former fixed 320 × 480 source limit. Non-stale decoded frames are converted and offered to the UI at the decoder's sustained rate; H.264 catch-up continues to decode but omits conversion for stale output. The fixture generator now covers native-display, 640 × 360 adaptive-size, fast-start/moov-last, and 1280 × 720 PSRAM-limit candidate files.

Unsupported-video errors list the ISO-BMFF video object type, sample-entry FourCC when available, and dimensions, followed by a prompt to add a decoder for that codec. If the playback demuxer rejects an MP4/3GP container before it exposes tracks, a bounded read-only ISO-BMFF probe still reports the `ftyp` brand plus the first video `stsd` sample-entry FourCC and dimensions when available. That failure path says a built-in H.264, H.263, or MPEG-4 Part 2 codec is already supported and identifies the container/layout as the issue; other codecs retain the add-a-decoder prompt. Fragmented-layout errors identify malformed fragment metadata, encryption, unsupported fragmented codecs, or PSRAM exhaustion rather than suggesting that an H.264 decoder is missing. The pinned minimp4 copy accepts the standard `avc2`/`avc3`/`avc4` H.264 sample entries alongside `avc1`, preserves `tkhd` track IDs, and permits valid zero-entry initialization tables; project-owned `FragmentedMp4Demuxer` handles fMP4 sample metadata. H.264 profile/configuration failures likewise say that the H.264 decoder is already present, so they are not mistaken for missing decoder support.

## SD cache rules

Caches live next to a source in a `.cache` directory. A complete cache is written first with `.tmp`, then renamed atomically. Rebuild an invalid, incomplete, obsolete, geometry-mismatched, or source-metadata-mismatched cache; never reuse it optimistically.

`MediaCache` owns this lifecycle. `CachePath` derives the directory, final path, and temporary path from a source path plus an internal `FileFormats` entry. `SourceFingerprint` is exactly the source file's 32-bit byte size and 32-bit last-modified value; both fields must match, including a zero timestamp. Format-specific codecs still validate their own magic, version, encoding, geometry, bounds, and payload size.

Cache I/O locks the shared SD/touch SPI bus for one filesystem operation or one bounded payload chunk. A lock miss is reported as busy so incremental UI work can retry on a later `tick()`; worker tasks release the lock and yield between chunks. No cache operation may hold the bus while decoding or processing a complete file.

A writer removes an abandoned temp before opening a new transaction, writes only the temp, flushes and closes it, then publishes it through rename. Replacing an old final and renaming the temp happen in one bus-locked commit operation. Failed and cancelled writers remove their temp on a best-effort basis, and the next writer or file cleanup removes any temp left by interruption.

Files cleanup enumerates every internal format from `FileFormats` and removes each final and temp sidecar incrementally. Individual file delete and rename clean up the old sidecars before changing the source. SDK 4 Lua apps follow the same rule: the app worker runs the incremental cleanup for a file before `esp.fs.rename` or `esp.fs.remove` changes it, and also for the rename target name. A single-file move into or out of secret storage incrementally copies every published final sidecar except legacy `.emga` files, which are removed with the source, to destination temporary sidecars, transforms its bytes with the destination's encryption mode, and replaces its source fingerprint with the staged file's fingerprint. After atomic primary-file promotion, Files publishes the prepared sidecars; if that promotion fails, it removes only those destination temporaries before retaining the source. Missing or failed sidecar transfers do not block the file move; their source copies and all `.tmp` artifacts are removed with the source, and the cache rebuilds on next use. Directory moves continue to exclude nested `.cache` directories; directory rename carries them with the directory.

### `.eth1`: Files thumbnail

Files thumbnails use an `ETH1` version-1 file: a 20-byte little-endian header followed by a 32 × 32 RGB565 payload (2,048 bytes). The header stores the source `FileFormats::Format` byte, thumbnail width and height, source byte size, source modification time, and payload size. The persisted source-format values must remain stable.

### `.em16`: static canvas

BMP, PNG, JPEG, and static WebP images use an `EM16` version-2 file: a 32-byte little-endian header followed by a fixed 480 × 320 RGB565 canvas (307,200 bytes). The header contains magic, version, encoding (`0`, little-endian RGB565), canvas geometry, source byte size, source modification time, payload size, and source width/height. The canvas contains the complete aspect-fit image and black letterboxing, so playback needs no image decoder. Version-1 files are rejected and rebuilt.

Eligible crop-zoom renders use a separate `.em16z` sidecar with the same validated version-2 header and RGB565 canvas. The fit and crop sidecars are independently cached, so switching either view can display without decoding or showing a loading indicator.

### `.emga`: legacy animated-image frames

Animated GIF and WebP used `EMGA` version 4 (packed RGB444 frames behind a 40-byte header) before they streamed. The firmware no longer reads or writes it. `FileFormats` keeps the extension, signature, and enum value so existing files stay hidden from listings, refused as Lua rename targets, and removed when Files deletes or renames their source. Secret-storage moves do not copy them.

### `.empa`: MP3 analysis

MP3 analysis uses an `EMPA` version-1, 36-byte little-endian header with no payload. It stores the header length, source byte size and modification time, compressed-audio start and end offsets, decoded sample count, sample rate, and channel count. A cache is accepted only when the fingerprint matches, offsets stay within the source, counts and sample rate are nonzero, channels are mono or stereo, and the first source frame agrees with the cached stream properties.
