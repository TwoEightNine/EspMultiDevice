# Media support and cache formats

## Central format registry

`src/media/FileFormats.h` is the sole source of truth for media classification. It resolves extensions, HTTP content types, and file signatures for BMP, PNG, JPEG, GIF, WebP, WAV, MP3, MP4/M4V, and 3GP/3GPP. Browser and Files must use it consistently.

`.eth1`, `.em16`, `.emga`, and `.empa` are internal cache files. They keep their image/audio grouping but are never openable or downloadable source media. Their extensions and internal classification live in `FileFormats`; new enum values must be appended because the ETH1 header persists the numeric source format.

## Image viewer

The viewer supports BMP, static non-interlaced PNG, baseline JPEG, simple/extended WebP, GIF87a/GIF89a, and animated WebP. Animated PNG and progressive JPEG are unsupported. Source images render aspect-fit on the 480 × 320 display.

From Files, tapping the left or right 48px edge moves to the previous or next supported image from the naturally sorted folder listing; navigation wraps. The Image Viewer itself has no navigation arrows.

Animated images fit a 320 × 240 photo viewport. Larger images are downscaled; smaller images are never upscaled. Animation support includes transparency, disposal/blending, and loop metadata. Insufficient PSRAM produces an error instead of synchronous SD fallback.

Visible image files may receive 32×32 Files thumbnails. They are worker-decoded, retained for the Files visit, and persist as validated `.cache/*.eth1` sidecars. Unsupported variants, oversized WebP input, and memory failures retain a normal file icon.

## Audio and recording

The recorder writes 16 kHz mono PCM WAV from the INMP441. The Audio Player streams mono or stereo integer PCM WAV and local MP3. Global volume is persisted through `AudioSettings` and ranges from 0 to 10 (default 5).

MP3 playback uses a sidecar cache at `<source-directory>/.cache/<file>.empa`. It stores validated compressed-audio bounds, format, and decoded sample count so later playback can skip a full MP3 scan while still decoding the original stream. It is valid only when source size and last-modified time match.

## Video

The Video Player opens MP4/3GP ISO-BMFF files up to 320 × 240. It accepts H.264 Baseline/Main/High, H.263, and MPEG-4 Part 2 video. Compatible AAC-LC, MP3, or AMR-NB (`samr`, 8 kHz mono) audio is sent to the I2S speaker.

The first open converts frames to an SD-backed animation cache before playback. `VideoH264` wraps pinned lib264; `VideoLegacy` exposes H.263 and MPEG-4 Part 2 through the same planar I420 callback contract, keeping the cache builder independent from source codecs. Both decoder paths need PSRAM. Use `tools/generate_legacy_video_fixtures.sh` and FFmpeg 9.0.1 to generate reproducible 176 × 144, 10fps legacy fixtures under `tests/video/fixtures/`.

## SD cache rules

Caches live next to a source in a `.cache` directory. A complete cache is written first with `.tmp`, then renamed atomically. Rebuild an invalid, incomplete, obsolete, geometry-mismatched, or source-metadata-mismatched cache; never reuse it optimistically.

`MediaCache` owns this lifecycle. `CachePath` derives the directory, final path, and temporary path from a source path plus an internal `FileFormats` entry. `SourceFingerprint` is exactly the source file's 32-bit byte size and 32-bit last-modified value; both fields must match, including a zero timestamp. Format-specific codecs still validate their own magic, version, encoding, geometry, bounds, and payload size.

Cache I/O locks the shared SD/touch SPI bus for one filesystem operation or one bounded payload chunk. A lock miss is reported as busy so incremental UI work can retry on a later `tick()`; worker tasks release the lock and yield between chunks. No cache operation may hold the bus while decoding or processing a complete file.

A writer removes an abandoned temp before opening a new transaction, writes only the temp, flushes and closes it, then publishes it through rename. Replacing an old final and renaming the temp happen in one bus-locked commit operation. Failed and cancelled writers remove their temp on a best-effort basis, and the next writer or file cleanup removes any temp left by interruption.

Files cleanup enumerates every internal format from `FileFormats` and removes each final and temp sidecar incrementally. Individual file delete and move-to-secret finish cleanup before removing the plaintext source. File rename invalidates the old sidecars before renaming; directory rename carries nested `.cache` directories with the directory.

### `.eth1`: Files thumbnail

Files thumbnails use an `ETH1` version-1 file: a 20-byte little-endian header followed by a 32 × 32 RGB565 payload (2,048 bytes). The header stores the source `FileFormats::Format` byte, thumbnail width and height, source byte size, source modification time, and payload size. The persisted source-format values must remain stable.

### `.em16`: static canvas

BMP, PNG, and JPEG images use an `EM16` version-1 file: a 24-byte little-endian header followed by a fixed 480 × 320 RGB565 canvas (307,200 bytes). The header contains magic, version, encoding (`0`, little-endian RGB565), canvas geometry, source byte size, source modification time, and payload size. The canvas contains the complete aspect-fit image and black letterboxing, so playback needs no image decoder.

### `.emga`: animation and video frames

Animated GIF/WebP and converted video use `EMGA` version 4. Its 40-byte little-endian header declares packed RGB444 encoding (`2`), fixed 480 × 320 display geometry, source size/time, source geometry, frame count, loop count, and cached draw geometry.

Each frame has a 32-bit little-endian delay in milliseconds followed by two 12-bit RGB444 pixels packed into three bytes (two bytes for an odd final pixel). This keeps 4,096 colors while using 25% less SD storage/read bandwidth than RGB565. The worker expands frames into an RGB565 PSRAM ring before TFT transfer. Source animation delays are clamped to at least 100 ms (10fps), and GIF/WebP frame transfers are batched so touch and close interaction remain responsive.

### `.empa`: MP3 analysis

MP3 analysis uses an `EMPA` version-1, 36-byte little-endian header with no payload. It stores the header length, source byte size and modification time, compressed-audio start and end offsets, decoded sample count, sample rate, and channel count. A cache is accepted only when the fingerprint matches, offsets stay within the source, counts and sample rate are nonzero, channels are mono or stereo, and the first source frame agrees with the cached stream properties.
