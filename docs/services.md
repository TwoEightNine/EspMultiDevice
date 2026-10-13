# Storage, audio, network, and device behavior

## Files and protected storage

SD storage uses the Arduino-ESP32 SPI backend at an explicit 25 MHz effective clock. Although the API accepts a requested frequency, this backend clamps non-UHS SPI cards above 25 MHz; do not treat a higher request as a throughput increase. Long reads and writes remain bounded and release the shared SD/touch `SpiBusLock` between transactions.

For controlled card testing, the disabled `RUN_SD_THROUGHPUT_BENCHMARK` boot diagnostic writes, flushes, reads, and verifies 4 MiB using 4 KiB transactions, prints separate write/read throughput to Serial, and removes its temporary file. Run it three times from cold boot before considering any I/O-buffer change. Keep the existing 4 KiB production chunks unless a relevant workload improves by at least 10% at the median and completes ten touch-active stress cycles without corruption, missed touch input, or card loss.

Files keeps an LRU of up to eight directories during one Files-screen visit. Listings are enumerated once and sorted directories-first with case-insensitive natural A–Z sorting. Returning to a cached folder avoids a filesystem read. Successful Files delete, rename, and secret-storage move actions update their affected complete listings in memory, so the current folder does not reindex; deleted folder descendants and stale thumbnails are discarded, while renamed folder caches are rebased. An affected listing that is still being enumerated is discarded and safely rebuilt. The cache survives a player or keyboard round trip and is discarded when Files closes or SD state changes; external SD changes are therefore authoritative after that reset.

The bottom pager has Previous, Page, and Next controls. Ordinary files and folders have a three-dot menu: Rename opens the keyboard with its complete name, and Delete requires a one-second hold. Folder deletion is recursive.

Holding the Home Files tile for one second opens a four-digit PIN input. The PIN selects the first byte of `/.configs/index`, a randomly generated 10,000-byte key file created only if absent. Names are hex-armored for FAT compatibility and names/source/cache bytes are XOR transformed. In normal folders, secret mode adds **Move to secret**; folder moves use a hidden staging directory before source removal. A single-file move prepares its published media sidecars in the destination cache directory with the destination's encryption mode and fingerprints them from the staged file, then publishes them after the primary file, so cache hits survive the move; an unavailable or failed sidecar is discarded without preventing the file move. Non-directory entries inside `/.configs` expose Rename, hold-to-delete, and **Move out of secret** actions; moving out writes a fully decrypted plaintext copy to `/Documents` through a hidden staging directory, auto-renaming collisions as `name (1).ext`, then removes the encrypted source only after commit. Secret folders remain browse-only.

This is intentional lightweight obfuscation, not authenticated encryption or secure password protection. An incorrect PIN opens unusable names/content. `SecretStorage` never overwrites a malformed existing key, and it locks whenever the SD card unmounts.

Files hides common macOS bookkeeping entries (`._*`, `.DS_Store`, `.Spotlight-V100`, `.Trashes`, `.fseventsd`, `.TemporaryItems`, and `.DocumentRevisions-V100`) without deleting them. They can still exist on the card and remain visible when Finder is configured to show hidden files.

## Mac Remote

The **Mac Remote** Home app starts the native GPIO 19/20 connector lazily as a HID-only device named `ESP Media Mac Remote`. Its screen owns the only action UI; leaving it releases keyboard modifiers and consumer-control reports, but the pinned Arduino-ESP32 USB stack remains enumerated until reset. USB mount, unmount, suspend, and resume callbacks publish state only, and the screen polls that state from `tick()` before enabling its complete action cells.

The fixed 3×3 actions are YouTube, full-screen screenshot (Shift-Command-3), lock (Control-Command-Q), previous, play/pause, next, output mute, volume down, and volume up. Media actions use standard USB consumer usages. YouTube uses a reusable non-blocking URL macro: Command-Space opens Spotlight, the controller waits 500 ms, types `https://www.youtube.com/` incrementally, waits 350 ms for the result, presses Down Arrow to select the top option, waits 100 ms, and presses Return. The macro is cancelled and all keys are released if the screen closes or USB disconnects. It depends on the standard Spotlight shortcut and an input source compatible with US-keyboard URL positions, but requires no macOS Shortcut, companion application, CDC channel, network command, or persisted device configuration.

Normal boot constructs the keyboard and consumer-control descriptors without constructing MSC. USB Drive transfer boot does the inverse. This keeps both host profiles single-purpose even if USB Drive is selected after Mac Remote has already started; the existing transfer request restarts before MSC is constructed.

## USB Drive mode

The **USB Drive** Home app records a one-shot Preferences flag and restarts into an isolated transfer boot. That boot does not create any media, browser, download, recording, or cache workers. It explicitly starts an MSC-only TinyUSB device on the native GPIO 19/20 connector. Before marking its medium present, it closes FatFs and reinitializes the Arduino-ESP32 3.3.8 SPI SD driver as an unmounted raw 512-byte block device. The physical card, including its MBR and FAT32 partition, is then exposed as writable USB mass storage named `ESP Media SD`; the filesystem volume label itself is preserved. UART0 remains available for logs and recovery through the separate USB-to-UART connector.

External apps are discovered under `/Apps` when the launcher opens or the card remounts. SDK 1, 2, and 3 app data lives under `/.system/appdata/<id>` on that card. Writes use a temporary file and rename, and reads/writes yield between 1 KiB SPI-locked chunks. App code cannot address other SD paths through the SDK. SDK 2 uses the shared speaker for bounded queued tones and the persisted global volume; its worker stops output when the app closes or the card is removed. SDK 3 also gives the app worker the shared microphone for bounded WAV capture and the speaker for WAV file playback inside the app data folder, under the same half-duplex rule as the built-in recorder. Reinserting the card allows the launcher to rescan. Package bounds and the public API are specified in [external-apps.md](external-apps.md).

While the Mac owns the card, device-side filesystem access is unavailable. Raw transfers validate the full byte range, handle partial-sector requests with read/modify/write, and release `SpiBusLock` between sectors for touch polling. The screen can be cancelled before host block access starts. Once active, eject the volume in Finder; the host stop/eject command withdraws the medium, deinitializes raw storage, and restarts into normal mode. A USB disconnect also returns to normal mode when the device remains externally powered. Suspend/resume is not treated as eject.

FAT32 limits a single file to less than 4 GiB. Abrupt USB/card removal or power loss during a write can corrupt the filesystem, just as with any external drive. PIN-offset protected entries are exposed only as their obfuscated on-card names and bytes; USB Drive never decrypts them and does not make `SecretStorage` secure.

The boot request is consumed before transfer setup, so pressing reset or power-cycling recovers directly to the normal UI if transfer mode cannot start. A failed TinyUSB start is reported separately from an SD I/O failure. After an unsafe removal, stop writing to the card and use macOS Disk Utility First Aid; if it cannot repair the volume, recover or back up readable data before reformatting the card as MBR/FAT32.

## Wi-Fi and time

`WiFiController` runs station mode and exposes `Disabled`, `Disconnected`, `Scanning`, `Connecting`, `Connected`, and `Failed`. It scans, stores/sorts up to 12 networks, connects with a 15-second timeout, disconnects, forgets credentials, and reconnects to saved ESP32 Wi-Fi credentials when allowed. Normal boot enables station mode when the splash appears and reconnects saved credentials without scanning an unconfigured device; the fixed three-second splash never waits for the connection. Before Wi-Fi's first initialization the controller selects Arduino-ESP32 static RX/TX buffers: this uses fixed driver memory instead of fragmenting the internal heap required for TLS. Do not change this mode after Wi-Fi has started; Arduino requires the radio to be off first. The Settings flow owns the UI; Home reflects connection state.

`Settings → Speed test` offers separate Wi-Fi and SD tests and keeps both
results only for the current screen visit. The Wi-Fi option performs an
explicit, user-started internet estimate over the current connection. It uses
Cloudflare's public `speed.cloudflare.com/__down` and `/__up` endpoints for five HTTP
time-to-first-byte probes followed by adaptive 256 KiB, 1 MiB, and 4 MiB
download and upload stages. A direction stops escalating after a three-second
stage or six seconds of completed transfers, and a complete fast run moves no
more than 10.5 MiB of payload. Reported latency is the median HTTP TTFB;
throughput is total completed payload bits divided by measured payload time.
Wi-Fi results never touch Preferences or SD.
During a run, the screen repaints only the changed signal, metric row, progress
region, status text, or action button; progress snapshots never redraw the
complete screen.

The test reuses an 8 KiB PSRAM buffer, falls back to 1 KiB of internal RAM,
overwrites it with zeroes before upload, and runs blocking network work on a
screen-owned opposite-core task. Closing the screen cancels by generation and
waits for the task and TLS client to stop.
The Cloudflare connection uses the same compatibility-mode unverified HTTPS
policy as Browser because it sends only synthetic bytes and no credentials or
user data. This does not make the measurement trustworthy against an active
network interceptor: interception can falsify the displayed result.

The SD option reuses `SdCardController`'s verified throughput benchmark on a
screen-owned opposite-core task. It writes 4 MiB of deterministic synthetic
data to `/.system/sd_benchmark.tmp` in 4 KiB SPI-locked transactions, reads and
verifies the same 4 MiB, reports write and read MiB/s, and removes the temporary
file. Progress is published no more often than every 125 ms. Cancellation is
checked between transactions, Back waits for cleanup and worker shutdown, and
an incomplete cleanup is reported instead of hidden. No result or test payload
is retained after the screen visit.

`TimeController` begins SNTP only while Wi-Fi is connected, marks time available after a valid epoch, and reports a fixed UTC+4 local clock. Change `UtcOffsetSeconds` if the product’s local-time requirement changes.

## Audio and workout

`MicController` captures INMP441 audio at 16 kHz on I2S port 0. Its optional physical power-switch sense line makes the recorder unavailable while the mic is off and stops a recording safely if power is removed. `SpeakerController` uses I2S port 1, starts muted, controls the amplifier enable pin, applies global volume, and keeps output buffering local to protect playback from filesystem/cache jitter. Its 16-descriptor DMA queue chooses frames per descriptor from the configured sample rate and retains about 200 ms at 8 kHz through 44.1 kHz, protecting speaker output during one bounded video decode without adding a one-second AMR delay. Its ordinary PCM write may wait for bounded space; `tryWritePcm16()` is the non-blocking contract, accepts no more than 256 mono frames, may return a partial or zero-length write, and treats zero as normal queue backpressure. `pauseOutput()` and `resumeOutput()` use `i2s_stop()`/`i2s_start()` while retaining the installed driver and queued DMA samples; `stop()` remains the destructive driver-uninstall boundary.

Recording and speaker playback never run concurrently because they share I2S clock and word-select pins. Stop/mute the relevant component before repurposing the bus.

### Production assistant

The assistant exists while its screen entry remains on the navigation stack.
The lightweight controller is registered at boot, but it creates the
worker/session only on fresh screen entry. Screens opened by Assistant cover
that entry without ending the authenticated session; a suspended UI tick keeps
the controller current while the worker maintains the idle WSS. Only the
Assistant app-bar X begins safe session exit and releases the WebSocket/codec
buffers.
Its controller publishes fixed-size immutable snapshots; the session worker alone performs
bootstrap, activation polling, WebSocket I/O, Opus conversion, microphone
capture, and speaker playback. Closing with X waits for a worker
acknowledgement after capture/playback are stopped, WSS and codecs are closed,
and credentials are wiped. Normal answer completion stops only the turn's I2S
paths and leaves the authenticated WSS and codecs alive for follow-up turns.

The microphone produces exact 960-sample, mono 16 kHz frames. The namespaced
OpusEncoder and OpusDecoder adapters delegate to the existing fixed-point
OpusCodec binding; the decoder accepts only 16 or 24 kHz mono output. The
speaker starts only after microphone shutdown. Normal TTS completion drains
the bounded downlink ring and I2S DMA before mute/stop; cancellation discards
queued audio immediately. The persisted global speaker volume remains
authoritative. While the Assistant stack entry exists, its controller samples
the optional microphone power-sense line on every visible or suspended UI tick. Turning the switch on
immediately restores the Ready microphone action without restarting; turning it
off updates the action, disarms Auto-listen, and safely cancels a pending or
active listening turn. Thinking and playback may finish because they no longer
use microphone input.

Command/event queue depths of four/eight, four uplink frames (240 ms), and eight
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
audio, captions, device-command payloads, and filenames are prohibited from
logs and snapshots. Firmware-update, MQTT, system, MCP, alert, custom, and
unknown instructions are ignored. Actionable custom RPC messages are
session-bound `device_command` requests from the authenticated local backend;
their request IDs, arguments, result schemas, and result IDs are strictly
validated on both sides. Version 1 supports `search_audio` and `play_audio`.
Capability version 2 also exposes
`get_time`, `get_wifi_status`, `get_wifi_signal`, `get_sd_status`,
`get_mic_status`, `get_battery_status`, `get_volume`, `get_led_status`,
`open_battery_details`, `set_volume`, `set_led_enabled`, `set_led_color`,
`search_images`, `open_image`, `search_videos`, `play_video`, `search_files`,
`show_file_info`, `open_folder`, and `search_web`. Version-1 devices continue
to receive only the audio tools. Status and setting actions run through the
UI-owned Assistant controller; settings report success only after persistence.
The battery command reports the most recent one-second smoothed measurement as
`voltage_mv`. Voltage alone does not establish state of charge or whether the
cell is charging. An absent or invalid sample returns `unavailable`.
Capability version 3 preserves that catalog, changes `search_files` matches to
exact SD-root-relative paths, and adds `list_directory`. The backend retains
the exact version-2 schemas for older clients.

While Ready, firmware sends an application ping every 60 seconds and requires
a pong within 10 seconds. Any reconnect clears the screen transcript and shows
the bounded reset notice because exact WSS dialogue context was lost; the
backend injects its persisted summary and up to 12 completed exact exchanges
into future replies. A five-exchange PSRAM ring
stores completed local captions (192 user bytes and 384 assistant bytes per
exchange), appends multi-sentence TTS captions, and discards cancelled drafts.
It remains the source of the newest live pair, but Assistant renders no
conversation history: user text is two-line ellipsized and assistant text pages
through three spoken lines every two seconds. The caption ring belongs to the
Assistant stack entry and survives screens opened by Assistant; it is wiped
only after X-close teardown. Covering the screen disarms Auto-listen so the
retained session stays idle. Holding the Assistant voice control
for 800 ms enables Auto-listen, and its next tap disables it. `mem_local_short`
persists a summary plus 12 completed exact exchanges per device in the backend.
Confirmed New conversation aborts active
work, sends the authenticated idempotent reset request, and clears local text
only after an `ok` acknowledgement.

The self-hosted assistant advertises `zh-CN` during protocol bootstrap for
upstream compatibility, but its TTS configuration explicitly tells the
upstream prompt enhancer to answer in the language of the latest utterance;
without that directive the enhancer silently forces Chinese. The assistant
uses a language-matching prompt for English, Simplified Chinese, and Russian.
OpenAI `gpt-transcribe` performs automatic transcription and GPT-5.6 Luna at
medium reasoning handles all chat, tool follow-up, and summary requests. Azure Speech
routes English to `en-GB-Ada:DragonHDLatestNeural`, Simplified Chinese to
`zh-CN-Xiaoxiao:DragonHDFlashLatestNeural`, and Russian to
`ru-RU-DariyaNeural`. The configured HD voices require the Speech resource to
be in `eastus`, `westeurope`, or `southeastasia`.
The backend resamples provider audio and pins its Opus downlink to mono 16 kHz
(960 samples per 60 ms packet), matching the firmware's native assistant frame
size rather than the upstream server's 24 kHz default.
Chinese and Cyrillic scripts select their corresponding configured voices;
Latin-script text selects the configured English fallback even after a Russian
or Chinese turn. Script-less streamed fragments (such as punctuation) retain
the preceding locale.
The backend state is unencrypted at rest; confirmed New conversation removes
both the summary and exact-turn memory for that device.
Caption buffers preserve only complete UTF-8 code points and replace malformed
input with `?` before the UI renders it.

### Agentic audio commands

The local backend exposes OpenAI structured tools only after the device's
hello advertises `device_tools`; MCP remains disabled. `search_audio` accepts
a UTF-8 title query of at most 96 bytes and returns at most five normal-SD
WAV/MP3 filename matches with opaque selection IDs. Filename matching is
case/punctuation/extension insensitive and deterministically prefers exact,
prefix, substring, then complete-token matches. It does not read tags, return
paths, enumerate PIN-offset protected storage, or enter hidden, `.system`, or
cache directories. Ambiguous results are sent back into the LLM context so it
asks the user to choose.
For "play any song", the backend searches with `*`; the device returns up to
five files in deterministic path order, and the backend plays the first one.

`play_audio` accepts only an unexpired selection ID from that session's latest
search. It rechecks the mounted source before returning acceptance, waits for
the terminal turn to reach Ready, and opens the existing auto-playing Audio
Player without TTS confirmation or ending the assistant session. The bounded selection set expires after 120
seconds and is erased on session close or reconnect.

The SD scanner uses each directory entry's absolute VFS path for recursive
lookup and playback; `File.name()` on Arduino-ESP32 contains only the basename.

The other local searches share audio's bounded, latest-only selection set and
normal-storage privacy filter. `search_files` includes ordinary files of any
type and folders with their displayed filename extensions; internal cache
formats remain excluded. Version 3 ranks both basenames and normalized complete
SD-relative paths, then returns the selected path so the assistant can answer
where a match is located. `list_directory` uses selection ID zero for root or
an unexpired folder selection from `search_files`, returns at most five direct
children per page, and accepts only its own active continuation cursor. Its
worker-owned directory iterator has a rolling 120-second expiry and closes on
completion, restart, cancellation, storage loss, session close, or reconnect.
Page order is the SD filesystem's iterator order rather than an alphabetical
sort.
Listings preserve the same hidden/system/cache/secret filter and never read
file contents. `show_file_info` returns name/type/size/modified time for a
selected file but never contents or path. Audio, image, video, folder, Battery
Details, and web launches wait for the terminal assistant turn to return to
Ready, then enter their existing screens while preserving the idle
authenticated session underneath. `search_web`
passes search words, never a URL, to the Browser's fixed DuckDuckGo Lite
entry; it is terminal and omitted from browser recents. The recorder writes
one `/voice_last.wav`, which remains available through `search_audio`.

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

The XPT2046 is polled without IRQ. Calibration learns a 4×4 mesh and stores it under `/.system/touch`; the calibration target view is full-screen, has no app bar or footer overlay, and places its targets on the absolute display corners and edges so those coordinates can be sampled. Captured samples are normalized from the current logical rotation into panel-native rotation-0 order before validation and persistence. The version-1 file format remains unchanged, so calibration created before the module's 180-degree physical rotation remains valid and newly captured calibration survives portrait/landscape changes. Before calibration, the controller uses a conservative noise floor. Its press/release debounce is 25/40 ms and normal event delivery happens only on state transitions.

The battery input expects a single Li-ion cell through a 100 kΩ / 100 kΩ divider. On normal boot, `BatteryHistoryService` stays idle through the three-second splash interval. At splash exit it asks `BatteryController` for the first sample synchronously, including the history-log write, and only then is Home entered and drawn. It samples every second afterward; Home, Battery Info, Assistant, and Lua apps consume that cached sample and do not trigger additional ADC reads. Each sample is the median of exactly three calibrated ADC readings.

`BatteryController` returns the raw median voltage and a smooth voltage formed from the most recent 30 raw samples, using the available shorter window during the first 29 seconds. The first smooth value, every decrease, and every rise of at least 0.15 V from the previously accepted smooth value are accepted. A smaller rise is held unless the newest raw sample is above 3.9 V. Held samples still enter the moving window, so sustained growth can eventually clear the 0.15 V threshold. The high-voltage exception permits charging-like growth but is not exposed as proof that the device is charging.

Settings → Battery renders raw and smooth voltage to three decimal places. Its chart plots only smooth voltage in green against a single left V axis, summarizes the complete current boot, maps `boot` to the left edge and `now` to the right edge, and chooses readable major time-grid intervals as the duration grows. Five horizontal labels retain the observed minimum-to-maximum range with 10% padding; a small fallback range keeps a flat trace drawable. The bounded in-memory summary progressively merges older samples into time-weighted points while preserving the full boot span.

At the first mounted-card sample of each boot, the service atomically creates `/.system/battery_history.bin` from `/.system/battery_history.bin.tmp`, replacing the previous boot's file. Version 2 appends one fixed-size record per sample containing elapsed boot milliseconds, raw millivolts, smooth millivolts, and a checksum. A version-1 file is rejected and replaced rather than migrated. The header identifies the format version, record size, sample interval, and boot start tick. Consumers process only complete records, so a trailing interrupted write is ignored. Every directory, header, and append operation locks the shared SD/touch SPI bus briefly. Sampling and the chart continue in RAM when the card is unavailable; persistence resumes once mounted, although missing samples are not backfilled.

Home displays the cached smooth voltage as `#.#V` inside the battery glyph. The fill is green above 3.66 V, red below 3.33 V, and yellow otherwise, including exactly 3.33 V and 3.66 V. No battery percentage is calculated or displayed because cell voltage varies with load and charging and is not a reliable state-of-charge measurement.

At boot the serial log (115200 baud) prints reset cause, CPU frequency, flash bytes, and total/free/largest PSRAM block. A reported brownout reset indicates supply sag rather than an application exception.
