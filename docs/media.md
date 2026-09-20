# Media support and cache formats

## Central format registry

`src/media/FileFormats.h` is the sole source of truth for media classification. It resolves extensions, HTTP content types, and file signatures for BMP, PNG, JPEG, GIF, WebP, WAV, MP3, MP4/M4V, and 3GP/3GPP. Browser and Files must use it consistently.

`.eth1`, `.em16`, `.em16z`, `.emga`, and `.empa` are internal cache files. They keep their image/audio grouping but are never openable or downloadable source media. Their extensions and internal classification live in `FileFormats`; new enum values must be appended because the ETH1 header persists the numeric source format.

## Image viewer

The viewer supports BMP, static non-interlaced PNG, baseline JPEG, simple/extended WebP, GIF87a/GIF89a, and animated WebP. Animated PNG and progressive JPEG are unsupported. Source images render aspect-fit on the 480 × 320 display.

Images open in informative mode with an opaque 48px top bar showing the source filename. Tapping the central image area toggles informative and full-screen modes; full screen hides the bar. The left and right 48px edges move through adjacent images, except that the informative bar owns its close target and, when available, its top-right zoom target. Eligible still images whose aspect ratio differs from the 480 × 320 screen ratio by more than 10% expose a centered crop-to-fill zoom toggle. Crop state persists while switching modes. Edge navigation preserves full-screen mode but resets zoom; every newly opened image starts aspect-fit in informative mode. Animated images do not expose crop zoom.

From Files, tapping the left or right 48px edge moves to the previous or next supported image from the naturally sorted folder listing; navigation wraps. The Image Viewer itself has no navigation arrows.

Animated images fit a 320 × 240 photo viewport. Larger images are downscaled; smaller images are never upscaled. Animation support includes transparency, disposal/blending, and loop metadata. Insufficient PSRAM produces an error instead of synchronous SD fallback.

Visible image files may receive 32×32 Files thumbnails. They are worker-decoded, retained for the Files visit, and persist as validated `.cache/*.eth1` sidecars. Unsupported variants, oversized WebP input, and memory failures retain a normal file icon.

## Audio and recording

The recorder writes 16 kHz mono PCM WAV from the INMP441. The Audio Player streams mono or stereo integer PCM WAV and local MP3. Global volume is persisted through `AudioSettings` and ranges from 0 to 10 (default 5).

MP3 playback uses a sidecar cache at `<source-directory>/.cache/<file>.empa`. It stores validated compressed-audio bounds, format, and decoded sample count so later playback can skip a full MP3 scan while still decoding the original stream. It is valid only when source size and last-modified time match.

## Video

The Video Player opens ordinary local MP4/3GP ISO-BMFF files with H.264 Baseline/Main/High, H.263, or MPEG-4 Part 2 video at any source dimensions the decoder can safely configure and PSRAM can hold. It also opens complete, seekable local fragmented MP4 (fMP4) files when their video is H.264 and their optional audio is AAC-LC. The fMP4 reader accepts `mvex`/`trex` defaults and standard `moof`/`traf`/`tfhd`/`tfdt`/`trun` sample layouts, retaining only a presentation-time list and the active per-track fragment run in PSRAM; it does not support seeking, live/growing files, manifests, encryption, changing sample descriptions, H.263/MPEG-4 Part 2 fragments, or non-AAC fMP4 audio. Source YUV remains decoder-owned in PSRAM and is aspect-downscaled to the 320 × 480 display; there is no source-resolution or minimum-FPS policy limit. The worker presents every successfully decoded frame it can sustain, including below 1 fps, while compatible AAC-LC, MP3, or AMR-NB (`samr`, 8 kHz mono) audio remains the real-time playback clock. Invalid dimensions, checked-allocation overflow, decoder limits, or PSRAM exhaustion produce a resource error with the requested geometry and free/largest PSRAM. Decoders enqueue mono PCM into a 16,384-frame PSRAM ring; startup requires at least 80 ms, normal refill begins below 80 ms and targets 160 ms, and non-blocking I2S submissions are capped at 256 frames per scheduler pass. A full I2S queue is backpressure, not a codec failure. The clock is active wall time clamped by submitted PCM duration, so an audio underrun also stops video progress.

Video streams directly from its source and never reads, writes, or removes an `.emga` sidecar. Existing video sidecars remain harmless orphaned cache files until normal Files cleanup; `.emga` remains the animation cache for GIF/WebP. `VideoH264` wraps pinned lib264; `VideoLegacy` exposes H.263 and MPEG-4 Part 2 through the same planar I420 callback contract. Both decoder paths need PSRAM and configure from the source geometry after checked arithmetic. The second-core worker owns the source, demuxer, decoders, display-sized frame buffers, PCM ring, and I2S operations. Audio drain, up to four compressed audio decodes, and one available video sample are interleaved in bounded scheduling quanta. Source samples share an 8 KiB PSRAM read window; each refill or larger-file chunk holds the shared SD/touch SPI lock only for one bounded transaction.

Videos open directly into a chrome-free 320 × 480 full-screen view and start automatically once two decoded video frames and the audio minimum are buffered. Full screen has no title, status text, progress, or playback control; tapping it requests embedded mode and pauses playback after the first replacement frame is ready. Any terminal opening or playback error, including an unsupported format, instead switches immediately to embedded mode so its app-bar close target is available. Embedded mode reserves the 48px app bar, titled with the source filename, and 32px progress strip, leaving a 320 × 400 viewport. Its 40 × 40px play/pause control is centered on the aspect-fit active video frame, while taps elsewhere in the video canvas return to full screen. The control's underlying video pixels are retained and restored before leaving embedded mode, so a paused full-screen view never shows a stale play icon. Resuming through the embedded control automatically returns to full screen after two seconds of playback. Both viewport geometries are centered on the physical display, not their available content area. Mode changes preserve the displayed frame and apply chrome and new geometry only when a replacement frame is ready; a paused/finished frame remains visible while its chrome changes. Frames remain aspect-preserving; unused space is black, so a 320 × 480 portrait source is pillarboxed rather than cropped or stretched. The worker safely replaces its two packed RGB565 frame slots when the viewport changes without restarting source, decoder, audio, or playback timing. Predictive frames are still decoded when late; conversion may be skipped beyond 250 ms lateness, but the newest decoded frame is forced out after 250 ms without a presentation so dropping cannot permanently freeze the image. Use `tools/generate_legacy_video_fixtures.sh` and FFmpeg 9.0.1 to generate reproducible 320 × 480, 30fps H.263, MPEG-4 Part 2, and H.264/AAC fixtures under `tests/video/fixtures/`.

The adaptive full-frame policy above supersedes the former fixed 320 × 480 source limit and 250 ms late-frame conversion throttle: all successfully decoded frames are converted and offered to the UI at the decoder's sustained rate. The fixture generator now covers native-display, 640 × 360 adaptive-size, fast-start/moov-last, and 1280 × 720 PSRAM-limit candidate files.

Unsupported-video errors list the ISO-BMFF video object type, sample-entry FourCC when available, and dimensions, followed by a prompt to add a decoder for that codec. If the playback demuxer rejects an MP4/3GP container before it exposes tracks, a bounded read-only ISO-BMFF probe still reports the `ftyp` brand plus the first video `stsd` sample-entry FourCC and dimensions when available. That failure path says a built-in H.264, H.263, or MPEG-4 Part 2 codec is already supported and identifies the container/layout as the issue; other codecs retain the add-a-decoder prompt. Fragmented-layout errors identify malformed fragment metadata, encryption, unsupported fragmented codecs, or PSRAM exhaustion rather than suggesting that an H.264 decoder is missing. The pinned minimp4 copy accepts the standard `avc2`/`avc3`/`avc4` H.264 sample entries alongside `avc1`, preserves `tkhd` track IDs, and permits valid zero-entry initialization tables; project-owned `FragmentedMp4Demuxer` handles fMP4 sample metadata. H.264 profile/configuration failures likewise say that the H.264 decoder is already present, so they are not mistaken for missing decoder support.

## SD cache rules

Caches live next to a source in a `.cache` directory. A complete cache is written first with `.tmp`, then renamed atomically. Rebuild an invalid, incomplete, obsolete, geometry-mismatched, or source-metadata-mismatched cache; never reuse it optimistically.

`MediaCache` owns this lifecycle. `CachePath` derives the directory, final path, and temporary path from a source path plus an internal `FileFormats` entry. `SourceFingerprint` is exactly the source file's 32-bit byte size and 32-bit last-modified value; both fields must match, including a zero timestamp. Format-specific codecs still validate their own magic, version, encoding, geometry, bounds, and payload size.

Cache I/O locks the shared SD/touch SPI bus for one filesystem operation or one bounded payload chunk. A lock miss is reported as busy so incremental UI work can retry on a later `tick()`; worker tasks release the lock and yield between chunks. No cache operation may hold the bus while decoding or processing a complete file.

A writer removes an abandoned temp before opening a new transaction, writes only the temp, flushes and closes it, then publishes it through rename. Replacing an old final and renaming the temp happen in one bus-locked commit operation. Failed and cancelled writers remove their temp on a best-effort basis, and the next writer or file cleanup removes any temp left by interruption.

Files cleanup enumerates every internal format from `FileFormats` and removes each final and temp sidecar incrementally. Individual file delete and rename clean up the old sidecars before changing the source. A single-file move into or out of secret storage incrementally copies every published final sidecar to destination temporary sidecars, transforms its bytes with the destination's encryption mode, and replaces its source fingerprint with the staged file's fingerprint. After atomic primary-file promotion, Files publishes the prepared sidecars; if that promotion fails, it removes only those destination temporaries before retaining the source. Missing or failed sidecar transfers do not block the file move; their source copies and all `.tmp` artifacts are removed with the source, and the cache rebuilds on next use. Directory moves continue to exclude nested `.cache` directories; directory rename carries them with the directory.

### `.eth1`: Files thumbnail

Files thumbnails use an `ETH1` version-1 file: a 20-byte little-endian header followed by a 32 × 32 RGB565 payload (2,048 bytes). The header stores the source `FileFormats::Format` byte, thumbnail width and height, source byte size, source modification time, and payload size. The persisted source-format values must remain stable.

### `.em16`: static canvas

BMP, PNG, JPEG, and static WebP images use an `EM16` version-2 file: a 32-byte little-endian header followed by a fixed 480 × 320 RGB565 canvas (307,200 bytes). The header contains magic, version, encoding (`0`, little-endian RGB565), canvas geometry, source byte size, source modification time, payload size, and source width/height. The canvas contains the complete aspect-fit image and black letterboxing, so playback needs no image decoder. Version-1 files are rejected and rebuilt.

Eligible crop-zoom renders use a separate `.em16z` sidecar with the same validated version-2 header and RGB565 canvas. The fit and crop sidecars are independently cached, so switching either view can display without decoding or showing a loading indicator.

### `.emga`: animated-image frames

Animated GIF/WebP use `EMGA` version 4. Its 40-byte little-endian header declares packed RGB444 encoding (`2`), fixed 480 × 320 display geometry, source size/time, source geometry, frame count, loop count, and cached draw geometry.

Each frame has a 32-bit little-endian delay in milliseconds followed by two 12-bit RGB444 pixels packed into three bytes (two bytes for an odd final pixel). This keeps 4,096 colors while using 25% less SD storage/read bandwidth than RGB565. The worker expands frames into an RGB565 PSRAM ring before TFT transfer. Source animation delays are clamped to at least 100 ms (10fps), and GIF/WebP frame transfers are batched so touch and close interaction remain responsive.

### `.empa`: MP3 analysis

MP3 analysis uses an `EMPA` version-1, 36-byte little-endian header with no payload. It stores the header length, source byte size and modification time, compressed-audio start and end offsets, decoded sample count, sample rate, and channel count. A cache is accepted only when the fingerprint matches, offsets stay within the source, counts and sample rate are nonzero, channels are mono or stereo, and the first source frame agrees with the cached stream properties.
