# Storage, audio, network, and device behavior

## Files and protected storage

SD storage uses the Arduino-ESP32 SPI backend at an explicit 25 MHz effective clock. Although the API accepts a requested frequency, this backend clamps non-UHS SPI cards above 25 MHz; do not treat a higher request as a throughput increase. Long reads and writes remain bounded and release the shared SD/touch `SpiBusLock` between transactions.

For controlled card testing, the disabled `RUN_SD_THROUGHPUT_BENCHMARK` boot diagnostic writes, flushes, reads, and verifies 4 MiB using 4 KiB transactions, prints separate write/read throughput to Serial, and removes its temporary file. Run it three times from cold boot before considering any I/O-buffer change. Keep the existing 4 KiB production chunks unless a relevant workload improves by at least 10% at the median and completes ten touch-active stress cycles without corruption, missed touch input, or card loss.

Files keeps an LRU of up to eight directories during one Files-screen visit. Listings are enumerated once and sorted directories-first with case-insensitive natural A–Z sorting. Returning to a cached folder avoids a filesystem read. Successful Files delete, rename, and secret-storage move actions update their affected complete listings in memory, so the current folder does not reindex; deleted folder descendants and stale thumbnails are discarded, while renamed folder caches are rebased. An affected listing that is still being enumerated is discarded and safely rebuilt. The cache survives a player or keyboard round trip and is discarded when Files closes or SD state changes; external SD changes are therefore authoritative after that reset.

The bottom pager has Previous, Page, and Next controls. Ordinary files and folders have a three-dot menu: Rename opens the keyboard with its complete name, and Delete requires a one-second hold. Folder deletion is recursive.

Holding the Home Files tile for one second opens a four-digit PIN input. The PIN selects the first byte of `/.configs/index`, a randomly generated 10,000-byte key file created only if absent. Names are hex-armored for FAT compatibility and names/source/cache bytes are XOR transformed. In normal folders, secret mode adds **Move to secret**; folder moves use a hidden staging directory before source removal. A single-file move prepares its published media sidecars in the destination cache directory with the destination's encryption mode and fingerprints them from the staged file, then publishes them after the primary file, so cache hits survive the move; an unavailable or failed sidecar is discarded without preventing the file move. Non-directory entries inside `/.configs` expose Rename, hold-to-delete, and **Move out of secret** actions; moving out writes a fully decrypted plaintext copy to `/Documents` through a hidden staging directory, auto-renaming collisions as `name (1).ext`, then removes the encrypted source only after commit. Secret folders remain browse-only.

This is intentional lightweight obfuscation, not authenticated encryption or secure password protection. An incorrect PIN opens unusable names/content. `SecretStorage` never overwrites a malformed existing key, and it locks whenever the SD card unmounts.

## Wi-Fi and time

`WiFiController` runs station mode and exposes `Disabled`, `Disconnected`, `Scanning`, `Connecting`, `Connected`, and `Failed`. It scans, stores/sorts up to 12 networks, connects with a 15-second timeout, disconnects, forgets credentials, and reconnects to saved ESP32 Wi-Fi credentials when allowed. Before Wi-Fi's first initialization it selects Arduino-ESP32 static RX/TX buffers: this uses fixed driver memory instead of fragmenting the internal heap required for TLS. Do not change this mode after Wi-Fi has started; Arduino requires the radio to be off first. The Settings flow owns the UI; Home reflects connection state.

`TimeController` begins SNTP only while Wi-Fi is connected, marks time available after a valid epoch, and reports a fixed UTC+4 local clock. Change `UtcOffsetSeconds` if the product’s local-time requirement changes.

## Audio and workout

`MicController` captures INMP441 audio at 16 kHz on I2S port 0. Its optional physical power-switch sense line makes the recorder unavailable while the mic is off and stops a recording safely if power is removed. `SpeakerController` uses I2S port 1, starts muted, controls the amplifier enable pin, applies global volume, and keeps output buffering local to protect playback from filesystem/cache jitter.

Recording and speaker playback never run concurrently because they share I2S clock and word-select pins. Stop/mute the relevant component before repurposing the bus.

The Stage 0 `SecureWebSocketClient` is the future assistant's bounded WSS layer over `NetworkClientSecure`. It requires a trust store and never uses the browser's HTTPS compatibility mode. Its connection handshake directs temporary TLS allocations to PSRAM so the internal-heap largest block remains available; future session code must perform this synchronous operation in worker-backed, non-UI work.

The Stage 0 `OpusCodec` adapter encodes only mono 16 kHz PCM in exact 60 ms (960-sample) frames. Its decoder is initialized explicitly for 16 or 24 kHz, with maximum output of 960 or 1,440 samples respectively. Codec state lives in PSRAM. This dependency foundation does not capture, stream, or play audio in normal firmware; a future assistant must continue to arbitrate the shared BCLK and WS lines half-duplex through the microphone and speaker controllers.

`AudioSettings` persists the speaker volume using ESP32 Preferences. `WorkoutScreen` has configurable work/rest periods in five-second steps from 5 to 600 seconds and plays audible stage cues using the shared speaker and volume setting.

## Touch, battery, and startup diagnostics

The XPT2046 is polled without IRQ. Calibration learns a 4×4 mesh and stores it under `/.system/touch`; before calibration, the controller uses a conservative noise floor. Its press/release debounce is 25/40 ms and normal event delivery happens only on state transitions.

The battery input expects a single Li-ion cell through a 100 kΩ / 100 kΩ divider. The controller measures every Home refresh and uses the median of exactly three ADC samples, with no cross-refresh smoothing or averaging. The displayed percentage only decreases during one controller runtime; runtime charging support is not available. Under Wi-Fi/display/speaker load the resting-cell curve can report a lower transient percentage.

At boot the serial log (115200 baud) prints reset cause, CPU frequency, flash bytes, and total/free/largest PSRAM block. A reported brownout reset indicates supply sag rather than an application exception.
