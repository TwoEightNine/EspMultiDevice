# Storage, audio, network, and device behavior

## Files and protected storage

SD storage uses the Arduino-ESP32 SPI backend at an explicit 25 MHz effective clock. Although the API accepts a requested frequency, this backend clamps non-UHS SPI cards above 25 MHz; do not treat a higher request as a throughput increase. Long reads and writes remain bounded and release the shared SD/touch `SpiBusLock` between transactions.

For controlled card testing, the disabled `RUN_SD_THROUGHPUT_BENCHMARK` boot diagnostic writes, flushes, reads, and verifies 4 MiB using 4 KiB transactions, prints separate write/read throughput to Serial, and removes its temporary file. Run it three times from cold boot before considering any I/O-buffer change. Keep the existing 4 KiB production chunks unless a relevant workload improves by at least 10% at the median and completes ten touch-active stress cycles without corruption, missed touch input, or card loss.

Files keeps an LRU of up to eight directories during one Files-screen visit. Listings are enumerated once and sorted directories-first with case-insensitive natural A–Z sorting. Returning to a cached folder avoids a filesystem read. Successful Files delete, rename, and secret-storage move actions update their affected complete listings in memory, so the current folder does not reindex; deleted folder descendants and stale thumbnails are discarded, while renamed folder caches are rebased. An affected listing that is still being enumerated is discarded and safely rebuilt. The cache survives a player or keyboard round trip and is discarded when Files closes or SD state changes; external SD changes are therefore authoritative after that reset.

The bottom pager has Previous, Page, and Next controls. Ordinary files and folders have a three-dot menu: Rename opens the keyboard with its complete name, and Delete requires a one-second hold. Folder deletion is recursive.

Holding the Home Files tile for one second opens a four-digit PIN input. The PIN selects the first byte of `/.configs/index`, a randomly generated 10,000-byte key file created only if absent. Names are hex-armored for FAT compatibility and names/source/cache bytes are XOR transformed. In normal folders, secret mode adds **Move to secret**; folder moves use a hidden staging directory before source removal. A single-file move prepares its published media sidecars in the destination cache directory with the destination's encryption mode and fingerprints them from the staged file, then publishes them after the primary file, so cache hits survive the move; an unavailable or failed sidecar is discarded without preventing the file move. Non-directory entries inside `/.configs` expose Rename, hold-to-delete, and **Move out of secret** actions; moving out writes a fully decrypted plaintext copy to `/Documents` through a hidden staging directory, auto-renaming collisions as `name (1).ext`, then removes the encrypted source only after commit. Secret folders remain browse-only.

This is intentional lightweight obfuscation, not authenticated encryption or secure password protection. An incorrect PIN opens unusable names/content. `SecretStorage` never overwrites a malformed existing key, and it locks whenever the SD card unmounts.

Files hides common macOS bookkeeping entries (`._*`, `.DS_Store`, `.Spotlight-V100`, `.Trashes`, `.fseventsd`, `.TemporaryItems`, and `.DocumentRevisions-V100`) without deleting them. They can still exist on the card and remain visible when Finder is configured to show hidden files.

## USB Drive mode

The **USB Drive** Home app records a one-shot Preferences flag and restarts into an isolated transfer boot. That boot does not create any media, browser, download, recording, or cache workers. It explicitly starts an MSC-only TinyUSB device on the native GPIO 19/20 connector. Before marking its medium present, it closes FatFs and reinitializes the Arduino-ESP32 3.3.8 SPI SD driver as an unmounted raw 512-byte block device. The physical card, including its MBR and FAT32 partition, is then exposed as writable USB mass storage named `ESP Media SD`; the filesystem volume label itself is preserved. UART0 remains available for logs and recovery through the separate USB-to-UART connector.

While the Mac owns the card, device-side filesystem access is unavailable. Raw transfers validate the full byte range, handle partial-sector requests with read/modify/write, and release `SpiBusLock` between sectors for touch polling. The screen can be cancelled before host block access starts. Once active, eject the volume in Finder; the host stop/eject command withdraws the medium, deinitializes raw storage, and restarts into normal mode. A USB disconnect also returns to normal mode when the device remains externally powered. Suspend/resume is not treated as eject.

FAT32 limits a single file to less than 4 GiB. Abrupt USB/card removal or power loss during a write can corrupt the filesystem, just as with any external drive. PIN-offset protected entries are exposed only as their obfuscated on-card names and bytes; USB Drive never decrypts them and does not make `SecretStorage` secure.

The boot request is consumed before transfer setup, so pressing reset or power-cycling recovers directly to the normal UI if transfer mode cannot start. A failed TinyUSB start is reported separately from an SD I/O failure. After an unsafe removal, stop writing to the card and use macOS Disk Utility First Aid; if it cannot repair the volume, recover or back up readable data before reformatting the card as MBR/FAT32.

## Wi-Fi and time

`WiFiController` runs station mode and exposes `Disabled`, `Disconnected`, `Scanning`, `Connecting`, `Connected`, and `Failed`. It scans, stores/sorts up to 12 networks, connects with a 15-second timeout, disconnects, forgets credentials, and reconnects to saved ESP32 Wi-Fi credentials when allowed. Before Wi-Fi's first initialization it selects Arduino-ESP32 static RX/TX buffers: this uses fixed driver memory instead of fragmenting the internal heap required for TLS. Do not change this mode after Wi-Fi has started; Arduino requires the radio to be off first. The Settings flow owns the UI; Home reflects connection state.

`TimeController` begins SNTP only while Wi-Fi is connected, marks time available after a valid epoch, and reports a fixed UTC+4 local clock. Change `UtcOffsetSeconds` if the product’s local-time requirement changes.

## Audio and workout

`MicController` captures INMP441 audio at 16 kHz on I2S port 0. Its optional physical power-switch sense line makes the recorder unavailable while the mic is off and stops a recording safely if power is removed. `SpeakerController` uses I2S port 1, starts muted, controls the amplifier enable pin, applies global volume, and keeps output buffering local to protect playback from filesystem/cache jitter. Its 16-descriptor DMA queue chooses frames per descriptor from the configured sample rate and retains about 200 ms at 8 kHz through 44.1 kHz, protecting speaker output during one bounded video decode without adding a one-second AMR delay. Its ordinary PCM write may wait for bounded space; `tryWritePcm16()` is the non-blocking contract, accepts no more than 256 mono frames, may return a partial or zero-length write, and treats zero as normal queue backpressure. `pauseOutput()` and `resumeOutput()` use `i2s_stop()`/`i2s_start()` while retaining the installed driver and queued DMA samples; `stop()` remains the destructive driver-uninstall boundary.

Recording and speaker playback never run concurrently because they share I2S clock and word-select pins. Stop/mute the relevant component before repurposing the bus.

### Production assistant

The Stage 2 assistant exists only while its screen is active. The lightweight
controller is registered at boot, but it creates the worker/session only on
screen entry and releases its WebSocket/codec buffers after safe screen exit.
Its controller publishes fixed-size immutable snapshots; the session worker alone performs
bootstrap, activation polling, WebSocket I/O, Opus conversion, microphone
capture, and speaker playback. Closing the screen waits for a worker
acknowledgement after capture/playback are stopped and credentials are wiped.

The microphone produces exact 960-sample, mono 16 kHz frames. The namespaced
OpusEncoder and OpusDecoder adapters delegate to the existing fixed-point
OpusCodec binding; the decoder accepts only 16 or 24 kHz mono output. The
speaker starts only after microphone shutdown. Normal TTS completion drains
the bounded downlink ring and I2S DMA before mute/stop; cancellation discards
queued audio immediately. The persisted global speaker volume remains
authoritative.

Four commands, eight typed events, four uplink frames (240 ms), and eight
downlink packets (480 ms) are the hard queue limits. Overflow closes the
connection. The opposite-core worker uses a 48 KiB internal-RAM stack and reports safe
stack/heap metrics. Stage 1 retained 21,076 bytes; production hardware
acceptance requires at least 8 KiB after ten turns and reconnect cycles.
Bootstrap diagnostics log only a bounded transport/HTTP/response class and
numeric HTTP status; they never log URLs, response bodies, or credentials.

Assistant bootstrap waits up to 30 seconds for secure time, then uses only the
injected self-hosted HTTPS endpoint and CA. Session URLs must be WSS with a DNS
hostname. Authentication rejection clears RAM credentials and restarts the
bootstrap flow. Other recoverable failures retry after 2, 4, 8, then 15
seconds. Tokens, Authorization, complete WSS URLs, session IDs, raw JSON,
audio, and captions are prohibited from logs and snapshots. Firmware-update,
MQTT, system, MCP, alert, custom, and unknown instructions are ignored.

The self-hosted assistant advertises `zh-CN` during bootstrap and its local
configuration uses a language-matching prompt plus the Edge-TTS-supported
`zh-CN-XiaoxiaoNeural` voice. Chinese speech receives Simplified-Chinese replies;
other speech is answered in its source language, but may have a Mandarin accent
because this backend uses one fixed voice.
Caption buffers preserve only complete UTF-8 code points and replace malformed
input with `?` before the UI renders it.

### Stage 0/1 validation history

`TlsMemoryScope` is the shared allocation adapter for mbedTLS handshakes. It
reference-counts the process-global allocator hook, places temporary
certificate/RSA/parser allocations in PSRAM with the ESP allocator as a
fallback, and restores the platform hook after the final overlapping scope.
It changes memory placement, never trust policy: `SecureWebSocketClient` still
requires a trust store and never uses the Browser's insecure HTTPS
compatibility behavior.

The Stage 0 `OpusCodec` adapter encodes only mono 16 kHz PCM in exact 60 ms (960-sample) frames. Its decoder is initialized explicitly for 16 or 24 kHz, with maximum output of 960 or 1,440 samples respectively. Codec state lives in PSRAM. This dependency foundation does not capture, stream, or play audio in normal firmware; a future assistant must continue to arbitrate the shared BCLK and WS lines half-duplex through the microphone and speaker controllers.

The migrated Stage 1 Xiaozhi probe requires a build-local HTTPS endpoint and
PEM CA through the ignored `LocalBackendConfig.h`; it has no public-hosted
default or fallback. It sends the
current upstream headers and system/board report, including SSID, RSSI,
channel, and local IP. Account login
never occurs on the device: when bootstrap returns activation instructions,
the serial diagnostic prints the bounded message and code while the user binds
the device through the Xiaozhi console. Activation version 1 polls the hosted
`/activate` endpoint with `{}` until accepted or timed out.

The returned WebSocket URL, token, and protocol version exist only in bounded
RAM buffers. The probe rejects plaintext WebSockets, malformed URLs, empty or
known placeholder tokens, and protocol versions outside 1–3. It first
advertises `mcp: false`; if the socket upgrades but no server hello arrives, it
retries once with the features object omitted. A valid hello is followed by an
immediate disconnect. The token, Authorization value, raw bootstrap body,
complete WSS URL, and server session ID are never logged or persisted.
After successful binding, an absent URL or token is retried through eight total
bootstrap attempts to tolerate propagation delay. A known placeholder/test
token fails immediately with a credential-free diagnostic category.

After its first successful hello, the probe stores temporary `assistant/s1_phase`
and `assistant/s1_mcp` bytes and restarts. The second boot reloads the same UUID,
performs a fresh bootstrap rather than reusing credentials, and requires a
second hello before clearing those temporary keys. This proves automatic
credential refresh across reboot; it does not add a production reconnect loop.

The 2026-09-15 hardware run completed normal account activation but the public
bootstrap then returned a recognized placeholder/test WebSocket token. The
hosted path is closed and no WSS attempt was made with that token. The follow-up
run against the Mac-hosted backend passed signed bootstrap, certificate-checked
WSS, the server hello with MCP disabled, and a second fresh bootstrap/hello
after automatic reboot. Further assistant work may target this self-hosted
protocol path, but no assistant UI or audio-session behavior is introduced by
the probe.

`AudioSettings` persists the speaker volume using ESP32 Preferences. `LedSettings` persists the initial DevKitC-1 RGB LED under the `led` Preferences namespace: an enabled flag and one of six palette values (red, green, blue, yellow, cyan, or magenta). Fresh installs select cyan but leave the LED off. `Settings → LED` applies toggles and palette changes immediately; disabling it preserves the chosen color for its next enable and both values restore at boot. `WorkoutScreen` has configurable work/rest periods in five-second steps from 5 to 600 seconds and plays audible stage cues using the shared speaker and volume setting.

## Touch, battery, and startup diagnostics

The XPT2046 is polled without IRQ. Calibration learns a 4×4 mesh and stores it under `/.system/touch`; the calibration target view is full-screen, has no app bar or footer overlay, and places its targets on the absolute display corners and edges so those coordinates can be sampled. Before calibration, the controller uses a conservative noise floor. Its press/release debounce is 25/40 ms and normal event delivery happens only on state transitions.

The battery input expects a single Li-ion cell through a 100 kΩ / 100 kΩ divider. Home refreshes the controller every five seconds. Each refresh takes the median of exactly three ADC samples. `BatteryController` retains the lowest median voltage seen during its in-memory session, so the voltage and percentage it returns never increase until the controller is recreated (normally at restart). The percentage is calculated from that same returned voltage, using linear interpolation through the measured 30-minute discharge checkpoints below; it is not calculated from a separate raw or inferred battery value. Home displays the percentage, without a `%` suffix, inside the battery glyph. Its battery fill is green at 66% and above, yellow from 33% through 65%, and red below 33%.

| Voltage | Percentage |
| ---: | ---: |
| 4.000 V | 100% |
| 3.900 V | 91% |
| 3.810 V | 82% |
| 3.750 V | 73% |
| 3.7025 V | 64% |
| 3.680 V | 55% |
| 3.640 V | 45% |
| 3.600 V | 36% |
| 3.550 V | 27% |
| 3.450 V | 18% |
| 3.350 V | 9% |
| 3.300 V | 0% |

At boot the serial log (115200 baud) prints reset cause, CPU frequency, flash bytes, and total/free/largest PSRAM block. A reported brownout reset indicates supply sag rather than an application exception.
