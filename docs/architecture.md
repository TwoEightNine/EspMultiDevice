# Architecture and component contracts

## Runtime composition

`esp_media.ino` is the composition root. It owns pin constants, constructs each controller and screen once, registers screens with `Navigator`, starts at `Splash`, and runs a generic loop. It must not call methods on a specific screen after registration. On normal boot, `SplashScreen` starts battery history after its three-second display interval; the first synchronous sample and log write finish before it replaces Splash with Home.

The one-shot USB Drive flag selects an isolated alternate composition. Transfer boot constructs only UART Serial, display, touch, SD/raw-block storage, Navigator, `USBMSC`, and `UsbDriveScreen`; it does not construct HID, network, browser, cache, decoder, recording, or playback components. Normal boot disconnects the ROM USB-Serial/JTAG pad. `MacRemoteController` is constructed only in the normal composition, but reconnects the pad and starts the HID-only TinyUSB device lazily when the Mac Remote screen first opens. TinyUSB has no supported runtime stop in the pinned Arduino core, so HID remains enumerated until reset while only the Mac Remote screen can send actions. The transfer flag is cleared as it is consumed, so the next reset returns to normal composition with native USB inactive until Mac Remote is opened again.

The loop forwards touch **state edges** only, updates SD hot-swap state, boot-scoped battery telemetry, Wi-Fi, and time, then asks `Navigator` to tick the stack. The active screen receives `tick(millis())`; covered entries receive the no-op-by-default `tickSuspended(millis())`. The state-edge rule prevents a held press that opened a screen from also activating a control in that screen.

```text
setup: services -> Navigator -> screens -> register -> Splash
loop:  touch edge -> active Screen::handleTouch()
       SD update -> Battery telemetry -> Wi-Fi update -> Time update -> Navigator::tick()
       Navigator: covered Screen::tickSuspended() -> active Screen::tick()
```

## Screens and navigation

All screens inherit `Screen`, which receives shared references to `Navigator`, `TouchController`, and `DisplayController`. Screen-specific services are supplied through that screen’s constructor. A screen owns its rendering, touch processing, asynchronous state, and lifecycle.

`Navigator` registers the 22 `ScreenId` values once and maintains a stack of at most eight entries. It supports `setRoot`, `push`, `replace`, and `pop`. Home selects an SD package ID through an `ExternalApp` screen payload; the reusable app-host screen revalidates that package before starting it.

- `onEnter()` is a normal entry.
- `onEnter(const ScreenData&)` receives a payload; the default delegates to `onEnter()`.
- `onExit()` runs when leaving the active screen.
- `releaseResources()` runs only when a stack entry is permanently popped or
  replaced. It releases screen-scoped buffers, sockets, queues, codecs, and
  task stacks; returning `false` defers navigation while a worker reaches a
  safe cancellation point. `push()` does not call it because the covered
  screen must be resumable.
- `onResume()` runs when a stacked screen becomes active again; its default redraws through `onEnter()`.
- `onResult(const ScreenResult&)` receives a result from `pop(result)`.
- `handleTouch()` receives a changed `TouchPoint`; `tick()` must stay non-blocking.
- `tickSuspended()` is only for lightweight servicing of a persistent owner
  while its stack entry is covered. It must not draw, consume shared
  one-recipient events, handle touch, or navigate; the default does nothing.

Use `ScreenData` for image, audio, video, text-viewer, text-input, and secret-files flows. Image payloads can carry adjacent image paths for edge navigation and the full-screen state to retain during an adjacent-image replacement; image zoom never transfers. Text-viewer payloads carry the source path plus normal/secret access mode. `ScreenResult::withText()` returns keyboard text to the caller. Do not introduce direct cross-screen calls for new navigation flows.

## Core services

| Component | Responsibility and contract |
| --- | --- |
| `DisplayController` | Initializes TFT_eSPI, exposes the display, can apply a runtime rotation, reports dimensions, and clears the panel. A caller changing its rotation must update `TouchController` with the same rotation and new dimensions before handling touches. |
| `TouchController` | Polls XPT2046 over SPI; debounces press/release, filters samples, maps to the display, and persists 4×4 mesh calibration at `/.system/touch`. Its rotation-aware mesh mapping preserves calibration when a screen changes display orientation, and new samples are normalized to panel-native order before persistence. |
| `SpiBusLock` | FreeRTOS mutex for the SPI wires shared by SD and XPT2046. Concurrent work locks only individual transactions. |
| `BatteryController` | Reads a divider-corrected cell voltage from the median of three calibrated samples and owns the 30-sample moving average. Small smooth-voltage rises below 0.15 V are held unless the newest raw sample is above 3.9 V; decreases and rises of at least 0.15 V are accepted. It reports voltage only because voltage cannot reliably identify state of charge or charging state. |
| `BatteryHistoryService` | Owns the boot-scoped one-second battery sample, cached raw/smooth voltage, bounded chart summary, and version-2 `/.system/battery_history.bin` append log. Sampling remains idle until `SplashScreen` calls `begin()` at splash exit, allowing the startup consumption spike to settle before the first ADC read. Home, Battery Info, Assistant, and Lua status consume this cache rather than sampling the ADC independently. Each SD transaction uses `SpiBusLock`. |
| `SdCardController` | Mounts, monitors, and remounts FAT storage. `update()` is called from the main loop and consumers obtain pending `Mounted`, `Removed`, or `MountFailed` state through `takeEvent()`. |
| `UsbMassStorageController` | Owns the TinyUSB MSC callbacks and host-visible state. In transfer boot it asks `SdCardController` to replace local FAT ownership with an unmounted raw SPI block device, exposes the whole card, and reports eject/disconnect/error state to `UsbDriveScreen::tick()`. |
| `MacRemoteController` | Owns the normal-composition TinyUSB keyboard and consumer-control HID reports. It starts on first Mac Remote entry, publishes connection state from USB callbacks, sends only bounded press/release pairs requested by the screen, and releases every report on screen exit. |
| `MediaCache` | Owns source-derived cache paths and fingerprints, bounded SPI-safe cache file operations, `.tmp` write transactions, atomic publication, abort cleanup, incremental sidecar cleanup, and staged sidecar relocation across encryption boundaries. Cache codecs retain their format-specific header and payload validation. |
| `LedSettings` | Owns the initial DevKitC-1's GPIO 48 WS2812-compatible RGB LED, its bounded RMT updates, and its persisted enabled/palette preference. `LedScreen` is its only UI caller. |

Without a card-detect pin, `SdCardController` probes an attached card every 500 ms and retries an absent card every three seconds. With card detect, mount attempts occur after the physical state changes. SD screens must react to events from their `tick()` methods, not assume a mounted card remains available.

## Concurrency and memory

The UI loop must continue to process touch and redraw while expensive work is under way. Files thumbnails, browser inline images, and animated-image cache loading use worker work on the other core; results are returned to the UI task for display updates. Do not let a worker redraw UI directly.

An external Lua app has one screen-owned worker on core 0. `AppRuntime` owns its Lua state, bounded app-file transactions, callback limits, and SDK 2 tone queue. SDK 1 publishes fixed-size row scenes; SDK 2 may publish bounded, complete drawing frames. Only the UI task renders them, within a viewport below the shared app bar, and forwards content-relative touch-down coordinates. The worker uses non-blocking speaker writes and stops output during exit or failure. Final navigation is deferred until the worker stops, then its task, queue, and Lua state are released.

Home owns a separate app-catalog worker. It scans bounded `/Apps` entries, validates runnable packages, and decodes exact 64×64 alpha PNG logos into PSRAM under generation cancellation. It publishes only complete catalogs; the UI task merges them with built-ins, sorts and draws them, and passes the selected package ID through `Navigator`. SD reads use `SpiBusLock`, and the worker never draws, consumes the shared SD event, or navigates. See [external-apps.md](external-apps.md) for the SD format and SDK.

Navigator records free/largest internal heap and free/largest PSRAM before and
after every successful final screen release. This is the hardware audit point
for screen-scoped ownership: a deferred release remains on the active screen
and continues `tick()` until cleanup is safe. Files thumbnail and Browser page,
image, and lifecycle tasks are created on screen entry/use and delete their
internal-RAM task stacks, queues, retained results, and buffers on final exit.

SD and touch share clock/data lines. Long media reads are deliberately chunked, acquire `SpiBusLock` only around a read, and release it between chunks. Media decoders and frame rings rely on PSRAM. The supported N16R8 profile is not optional for the full animated-image/video feature set.

USB MSC callbacks run outside the UI task. They never draw, navigate, or restart. Each 512-byte SD sector transaction takes `SpiBusLock` and releases it before the next sector so touch polling can proceed. Safe eject, disconnect, and I/O failures are published as state and acted on by the USB Drive screen. Local FatFs is fully unmounted before raw media becomes present to the host; normal and host filesystem ownership never overlap.

`MediaCache` is constructed once in the composition root and shared by Files, Image Viewer, and Audio Player. Its reader, writer, cleanup, and relocation operations report busy separately from failure. Screen work retries incremental cleanup or relocation from `tick()`, and cache workers yield between bounded transfers; the service never decodes media, paints, or navigates.

`TextViewerScreen` owns read-only paging for local text files. It retains only
the visible wrapped page plus 64 recent page offsets. Source reads are at most
512 bytes per `tick()` and each seek/read transaction takes `SpiBusLock`, so a
large file never needs to fit in RAM and touch polling keeps running. Forward
page boundaries are discovered while rendering; a previous page outside the
recent-offset cache is recovered by an incremental rescan from the nearest
known boundary. The app bar uses the source filename, the footer uses shared
page arrows, and the center status reports the page number plus byte progress.
Normal files restart at page one after an SD remount. Secret files cannot be
reopened after removal because the main loop deliberately locks
`SecretStorage`; the user must close the viewer and unlock Files again.

`VideoPlayerScreen` owns only video UI and TFT presentation. Its screen-scoped `VideoStreamWorker` runs on the core opposite the Arduino UI loop and exclusively owns the source `StorageFile`, ISO-BMFF demuxer, video/audio decoders, 16,384-frame mono PCM ring, packed PSRAM frame slots, and I2S speaker calls. It drains at most 256 PCM frames with a zero-timeout I2S call, decodes at most eight compressed audio packets when restoring its reserve, and decodes at most one available video sample in a scheduling pass; it yields after a video decode or an 8 ms work quantum. Audio backpressure therefore cannot pace video decoding. Playback time is active wall time clamped by submitted PCM duration, so underruns stop progress rather than making video run ahead. Pause stops and resumes the installed I2S peripheral without destroying its queued DMA audio.

The worker computes an aspect-fit active rectangle from source metadata for the current UI viewport, with both viewports centered on the physical display. Its two RGB565 results contain only packed active pixels; when the player switches between full-screen and embedded viewports or rotates the panel, the worker waits for the UI to release any borrowed slot, replaces those packed slots, and continues source, decoder, and audio-clock ownership without a restart. The player changes TFT and touch rotation together: upright portrait is rotation 2 for the physically inverted module and the on-demand counter-clockwise landscape presentation is rotation 3. A playing full-screen tap defers pause until one replacement embedded frame is ready. The UI retains the current canvas until it can draw that new frame, then applies the new chrome; it retains and restores the small video area beneath the centered playback control before removing embedded chrome, so a paused or finished frame remains intact. Landscape embedded mode omits the app bar/title and overlays its playback/progress controls plus a bottom-left orientation-exit cell on the video canvas; landscape fullscreen is chrome-free. All terminal video errors and final screen exits restore portrait before presenting other chrome. Resuming from the embedded control schedules a two-second return to full screen. A late H.264 output is still decoded to preserve its reference chain, but its RGB conversion and publication are skipped when it is more than 250 ms behind the audio clock; this is the only H.264 catch-up drop. Commands are generation-tagged and cancellation is acknowledged before the screen is released. A shared 8 KiB PSRAM source window serves audio and video samples; a miss refills it under one bounded `SpiBusLock` transaction, while larger reads release the lock between 8 KiB chunks. For complete local fMP4, the worker holds a sorted video presentation-time list plus one PSRAM-backed active fragment run per compatible track; its two independent cursors parse `moof` metadata on demand and never index compressed media. Worker scheduling counters stay local and a copied UI snapshot is published at most every 100 ms except phase, error, or buffering changes. The worker does not use `MediaCache` or create video sidecars.

Adaptive full-frame playback supersedes the former fixed source-size policy: every non-stale decoded frame is scaled and offered to the UI at the decoder's sustained rate, while H.264 outputs that cannot meet the audio clock are decoded without conversion so their references remain valid.

## Browser internal boundaries

`BrowserScreen` remains the Browser screen's Navigator and UI-task coordinator.
Its browser-internal records and limits live in `BrowserTypes.h` and
`BrowserLimits.h`, so bounded contracts are shared rather than duplicated in a
screen declaration.

`BrowserTransport` owns the complete synchronous HTTP transfer boundary: its
private HTTP client pair and serialization mutex, same-origin HTTP/1.1
keep-alive state, DNS and compatibility-mode TLS policy, request/response
parsing, redirects, body framing, timeouts, and byte
limits. It receives request policy, a cookie provider, and a transfer observer
rather than Navigator, touch, or display dependencies. It drives an injected
`BrowserDownloadStore` and returns one terminal result containing the final
URL and either retained text or a verified committed download path.
`BrowserPersistentState` owns recents, favorites, cookies, and the plaintext
browser-state file; it has no UI dependency. Neither transport nor persistence
paints the display or navigates.

`BrowserPageLoader` owns foreground page work above that synchronous transport:
its one-entry request/result handoff, other-core task, generation cancellation,
session-page cache access, UTF-8 normalization, and normalized cache writes. It
coalesces progress into a latest-value record for `BrowserScreen::tick()` and
returns a typed terminal result containing text, a download path, or an error.
It may read and update browser cookies through `BrowserPersistentState`, but it
never parses a `BrowserDocument`, draws, updates history/recents, or navigates.

`BrowserDownloadStore` owns response media classification, safe download names,
the single active `/Downloads` transaction, bounded incremental SD writes,
signature and ISO-BMFF validation, and atomic commit or cleanup. It is injected
into `BrowserTransport`, which owns HTTP body framing, cancellation polling,
and progress callbacks. The store contains all download-specific `SD`,
`SpiBusLock`, and `FileFormats` policy.

Browser transport allocates one bounded receive buffer in PSRAM and falls back
to a 512-byte internal allocation only when necessary. The serialized transport
is its sole user. HTTPS handshake certificate/RSA/parser allocations also use
PSRAM through the shared scoped mbedTLS allocator; this changes allocation
placement only and does not change the Browser's insecure compatibility trust
policy. Download and session-page-cache operations take the shared
SPI lock only for bounded 4 KiB SD transactions, release it, and yield.
`BrowserTransport::closeWhenIdle()` is UI-safe: it closes an idle connection
immediately or asks the serialized worker to close after cancellation, so the
UI never reaches into a live client object.

`BrowserSessionCache` owns the Browser session's bounded page/image index,
LRU accounting, and cache mutex. Its disposable files remain under
`/.system/browser_session/`; the Browser lifecycle decides when to begin or
end a session, while cache state is never duplicated in the screen.

`WiFiSpeedTestScreen` owns separate one-shot opposite-core workers for its
Wi-Fi and SD modes. The Wi-Fi worker alone owns its Cloudflare TLS connection
and bounded transfer buffer. The SD worker invokes the cancellable,
verification-preserving `SdCardController` benchmark, which releases
`SpiBusLock` between 4 KiB transactions. Both publish fixed copied snapshots;
only the UI task draws or navigates, and mode switching is disabled while a
worker is active. Back navigation generation-cancels the active work and
remains on the screen until the task, mutex, TLS/buffer resources when present,
and SD temporary-file cleanup have finished.

`BrowserLifecycleWorker` owns the Browser's one-entry lifecycle queue, worker
task, generation invalidation, and completion signal. It invokes a narrow
Browser callback for cache cleanup and persistent-state loading; the callback
never draws, while `BrowserScreen::tick()` consumes the completion signal and
redraws on the UI task. Browser workers are session-scoped rather than created
at firmware boot. Closing cancels their generations, waits without blocking the
UI for active work to unwind, then releases all three task stacks and transport
storage before Navigator returns to the previous screen.

`BrowserDocument` owns the Browser's bounded parsed-document storage: rows,
links, forms, styles, page boundaries, document background, and embedded-image
descriptors. It also owns display-independent pagination, thumbnail reflow,
document hit testing, and form value/reset/toggle/payload behavior. The screen
supplies a layout snapshot and normalized-URL callback, then consumes the
document through read-only records and explicit mutation methods; it neither
declares nor reaches into a duplicate document model. Embedded-image pixels in
the document are borrowed, UI-task-only rendering references.

`BrowserImageWorker` owns the Browser's JPEG/PNG/GIF thumbnail decoder, bounded
job/result queues, other-core task, generation cancellation, image transfer and
session-cache workflow, and all temporary or completed pixel allocations. It
consumes copied jobs created by `BrowserDocument`, returns copied result records
to `BrowserScreen::tick()`, and never draws, navigates, reflows, or mutates the
document. The screen marks accepted borrowed pixels before attaching them on the
UI task. Destructive document replacement invalidates the generation and frees
accepted buffers; in-page user cancellation invalidates only unfinished work so
already drawn previews remain valid.

Before submitting a page load, `BrowserScreen` invalidates embedded-image work
and waits for any active decoder through successive non-blocking `tick()` calls.
The shared `BrowserTransport` mutex remains the final serialization boundary for
page and preview HTTP transfers.

## Assistant production boundary

Stage 2 registers AssistantScreen only in normal composition. The Home AI tile
opens it; USB Drive composition never creates assistant services.
AssistantScreen owns drawing, touch, navigation, snapshot polling, and the
X-only safe-close handshake. AssistantAvatar owns the lower 320×240 avatar pane,
including clearing it before a new asset set is available, and paints one
prepared 320×240 RGB565 frame. AssistantAvatarLoader is a screen-scoped worker that
decodes the five exact PNG assets from the selected `/.system/ai_avatars/<dir>/` into
PSRAM; it never draws or updates assistant state. It also decodes the required,
non-interlaced 320×432 `/.system/ai_avatars/background.png` into PSRAM; the UI
draws it one-for-one at (0,48). `/.system/ai_avatars/avatars.config`
contains optional `Name:dir` entries. The Assistant overflow menu always offers
Default (`default`) followed by at most nine valid entries; missing, unreadable, or
malformed configs expose Default only. Selection is screen-scoped, resets to Default
on a fresh entry, and preserves the previous artwork until the selected replacement
frame is decoded, so it does not clear captions or interrupt a dialogue. SD
reads remain bounded by `SpiBusLock`, and the UI task publishes frames to the
TFT. Missing, invalid, or temporarily unavailable background artwork falls
back to black; SD removal/remount clears and reloads it so stale artwork cannot
remain. Avatar alpha is composited over the matching 320×240 slice of that
background, rather than a black pane.
The portrait layout uses a 48px app bar and the 320×432 background from y=48
through the bottom edge. The background owns the visible bubble shapes; the UI
draws only the latest live user/reply pair, never transcript history or page
arrows. The user caption is in (22,69)–(297,116), wraps to two 21px lines, and
ellipsizes overflow. The assistant caption is in (22,148)–(297,219), shows at
most three spoken lines, starts at its first page, and advances by three lines
every two seconds to the final page without restarting on each sentence. Han
text selects the assistant-only 18px Noto Sans CJK SC face. A 60×60 fill-only
voice control at (130,396) overlays the avatar: blue microphone while ready,
green stop while listening, blue cancel during active work, and gray slashed
microphone when unavailable or disabled. The overflow menu contains a two-tap
New conversation confirmation and the existing Avatar submenu; its rows begin
immediately below the app bar. Holding the voice control inside its bounds for
800 ms enables screen-session Auto-listen and produces a 160 ms cyan pulse;
the next tap disables it without applying that tap's normal action.

AssistantController is the UI-facing boundary. Its commands include manual
turn start/cancel, Auto-listen, authenticated conversation reset, retry, and
stack-entry activation/final deactivation. Covering Assistant with a screen it
opened does not deactivate the controller: its suspended tick continues to
drain worker events and reconcile connectivity without drawing, navigating, or
consuming SD events. Covering the screen disarms Auto-listen so the retained
session stays idle. Returning to Assistant reuses the authenticated WSS,
transcript, and controller generation. Only tapping the Assistant app-bar X
starts the safe deactivation handshake and permanently pops its stack entry.
Callers receive a value copy of the
mutex-protected AssistantSnapshot and copy transcript entries through the same
mutex. While its stack entry exists, the controller also reconciles the physical
microphone power-sense line into that snapshot on every visible or suspended UI tick; loss of power
disarms Auto-listen and generation-cancels pending or active capture without
tearing down the authenticated session. The stack-scoped PSRAM ring retains
five completed exchanges plus one draft: user text is UTF-8-safe at 192 bytes
and assistant text at 384 bytes.
Completion of a sixth exchange evicts only the oldest; cancellation discards
the draft. Snapshots and events never contain credentials, Authorization
headers, complete WSS URLs, device/session IDs, or raw JSON.

XiaozhiSession is created only while the Assistant stack entry exists, whether
visible or covered by a screen that it opened; it owns
the opposite-core 48 KiB worker with an internal-RAM stack (required while network code disables the flash cache), bootstrap lifecycle, verified WebSocket,
protocol and codec adapters, sensitive buffers, and every microphone/speaker
operation. Its command/event queues hold 4/8 records. Their payload storage,
bounded credentials, and audio rings are in PSRAM; the queue control blocks stay
in internal RAM. The rings hold four 60 ms uplink PCM frames and eight downlink Opus packets. A full
queue terminates that connection with a recoverable typed error. WebSocket
ingress yields after each complete frame so normal TTS bursts are backpressured
by playback instead of overflowing the four-event staging queue. Every command
and event carries a generation; cancellation changes the requested generation,
stale results are dropped and audio rings are wiped. Turn cleanup drains or
discards playback and stops both I2S paths without closing WSS or destroying
codecs. Only session cleanup—screen release, failed connection, or reconnect—
closes WSS, destroys codecs, wipes credentials, and acknowledges safe resource
release to the controller.

XiaozhiBootstrap is the renamed Stage 1 implementation and owns only
certificate-checked activation/bootstrap HTTP and bounded response parsing.
AssistantIdentity is the renamed identity service and preserves the
MAC-derived device ID plus the existing assistant/client_uuid NVS key.
XiaozhiProtocol exclusively builds/parses JSON and v1/v2/v3 audio envelopes.
It always advertises features.mcp false plus `features.device_tools: true`,
accepts only mono Opus with 60 ms frames (16 kHz uplink and 16/24 kHz
downlink), and ignores MCP, system, firmware-update, MQTT, alert, custom, and
unknown messages. It accepts only authenticated, session-matching
`device_command` messages while a turn is Thinking. The backend's early TTS
`start` only marks speech as pending; the first audio packet starts the speaker
and moves the turn to Speaking. Final STT is accepted only
while Listening; a duplicate arriving after Thinking or Speaking begins is
stale and ignored, so it cannot interrupt the TTS/audio drain. Auto VAD's final
STT already ends capture, so the device does not send another listen-stop that
would trigger a duplicate ASR job. The strict v1 commands are
`search_audio {query}` and `play_audio {selection_id}`; each has a nonzero
request ID and receives one correlated `device_result`. Malformed fields,
unknown commands, duplicate IDs, and mismatched sessions are never executed.
It also owns bounded ping/pong and authenticated conversation-reset envelopes.
Reset validates the `reset` action, nonzero 32-bit request ID, and `ok`/`error`
result; generation/request matching prevents stale acknowledgements from
clearing captions.

The connection path is NeedsWiFi → Bootstrapping → Connecting → Ready, with
optional NeedsActivation → Bootstrapping. Every turn then follows Ready →
Listening → Thinking → Speaking → Ready on that same authenticated WSS.
Cancellation aborts only the active turn. Auto-listen defaults off on every
entry; enabling it from Ready starts immediately and completed playback starts
the next turn, while disabling it cancels Listening but lets Thinking/Speaking
finish. Idle Ready sends a heartbeat every 60 seconds and requires pong within
10 seconds. Recoverable reconnects retain the 2/4/8/15-second backoff, clear the
local transcript, and show `Connection restored; captions reset` because the
exact server Dialogue was replaced. The server retains its own per-device,
versioned memory record: a summary plus at most 12 completed user/assistant
exchanges. It re-injects that bounded context into a later connection, while
New conversation atomically removes both fields. This backend state is not
encrypted at rest; it is never copied to firmware or its UI transcript.

The injected AssistantBackendConfig borrows a bootstrap URL, activation URL,
and PEM trust anchor. Production accepts HTTPS bootstrap/activation and WSS
session URLs only, requires a DNS hostname for SNI, rejects IP literals,
userinfo, fragments, malformed ports and empty trust, and never inherits the
browser compatibility TLS policy. A missing ignored local configuration
compiles into a non-retryable configuration error. No production default or
fallback points at a public Xiaozhi service.

The worker enforces 15-second bootstrap and WSS-connect limits, a 10-second
server-hello limit, 45-second listening inactivity, 60-second thinking,
15-second speaking packet inactivity, 120-second speaking total, and
180-second total turn, plus a 10-second reset acknowledgement limit. Microphone capture stops before speaker startup;
playback drains, mutes, and stops before Ready. Logs contain only generation,
phase, safe error category, bounded queue counts, heap/PSRAM, and stack
headroom.

`AssistantDeviceTools` is worker-owned and has no Navigator dependency. It
searches normal mounted SD storage only: each directory operation takes the
shared SPI lock briefly, yields between entries, and excludes secret, hidden,
system, and cache paths. WAV/MP3 classification comes only from `FileFormats`.
It returns at most five normalized filename matches with opaque IDs; version-3
file searches additionally expose exact user-facing SD-relative paths while
never accepting a path back as authority. Selections expire after 120 seconds
and are revalidated before play.
An accepted play request is first returned to the backend, then published as a
generation-tagged `LaunchAudio` event. `AssistantScreen` retains the launch
until the terminal turn reaches Ready, which proves both assistant I2S paths
are stopped, then pushes `AudioPlayer` with the retained local path. The
authenticated session remains idle underneath the player and is reused when
the player closes.

Version-2 device tools reuse the same bounded scanner for images, videos, and
ordinary files/folders; every search invalidates the preceding selection set.
The worker forwards status and persisted-setting requests through a correlated
UI action handled by `AssistantController`. Accepted media, folder, Battery
Details, and web-search launches publish generation-tagged events only after
their device result was sent; `AssistantScreen` waits for the terminal turn to
reach Ready and pushes the selected screen with a `Navigator` payload without
deactivating the session. Files accepts a
normal-folder payload, while Browser accepts only a search-query payload and
builds its fixed provider URL internally. Neither worker nor backend receives
navigation authority.

Version 3 adds a separate paged directory-listing context. Selection ID zero
opens the visible SD root, while IDs 1–5 must be unexpired folder selections
from `search_files`. The context owns one open directory iterator, its path,
expected continuation cursor, optional prefetched entry, and rolling
120-second expiry. It returns at most five direct-child SD-relative paths per
page in filesystem iterator order, keeps every iterator transaction under
`SpiBusLock`, and closes on end,
restart, cancellation, SD loss, session close, or reconnect. Listing produces
only a correlated device result; it never publishes a navigation event.

## Assistant dependency and compatibility history

Stage 0 adds no assistant screen, session controller, Xiaozhi activation, or
Xiaozhi network traffic. `SecureWebSocketClient` is the future assistant's
narrow synchronous transport adapter: it implements bounded RFC 6455 framing
over `NetworkClientSecure`, accepts a host/port/path endpoint, bounded custom
headers, and a mandatory PEM CA or ESP certificate bundle; it exposes only
connect, poll, text/binary send, close, state, and copied bounded events. Its
connection handshake puts temporary TLS allocations in PSRAM. It never uses
`BrowserTransport`, whose browser-specific HTTPS compatibility behavior is
unsuitable for authenticated assistant traffic.

`OpusCodec` is the future audio wire-format boundary. It accepts exactly 960
mono PCM16 samples for each 60 ms, 16 kHz encode operation, caps packets at the
Opus 1,275-byte maximum, and decodes one packet into a bounded 960-sample 16 kHz
or 1,440-sample 24 kHz output. Codec state is allocated in PSRAM. Stage 1 must
keep microphone capture and speaker playback half-duplex and place any
network/codec loop on non-UI work rather than blocking `Screen::tick()`.

Stage 1 added a compatibility diagnostic, not assistant behavior. That
diagnostic is now migrated to the production component names.
`AssistantIdentity` owns the assistant's stable identifiers: the device ID is the
lowercase Wi-Fi station MAC and a random UUIDv4 is generated once in NVS under
`assistant/client_uuid`. A malformed stored UUID is a hard error because
silently replacing it would change the server-side device identity.
`XiaozhiBootstrap` owns the certificate-checked HTTP POST, upstream-style
board report, bounded response, and parsing of only activation and WebSocket
configuration. Its endpoint and PEM trust anchor can be supplied by a
build-local configuration. It has no hosted-service default or fallback. It
rejects non-HTTPS bootstrap configuration, never exposes firmware
or MQTT response data, and never logs the raw response or credentials.

The Stage 1 probe is selected only by
`ESP_MEDIA_XIAOZHI_COMPATIBILITY_PROBE`. It runs in a dedicated 48 KiB task,
uses the existing secure WebSocket adapter, sends one protocol `hello`, waits for a
validated server `hello`, and disconnects. No audio, MCP request, system
command, or other server message is acted on. The normal composition root now
reaches these migrated services only through `AssistantController` and
`XiaozhiSession`.

The public-hosted gate failed after successful account binding because bootstrap
returned a placeholder/test WebSocket token. The architecture therefore does
not proceed to an assistant screen or production session against the public
service. The same diagnostic subsequently passed against the local
`xinnan-tech/xiaozhi-esp32-server` backend behind Caddy-managed HTTPS/WSS:
signed bootstrap credential, WebSocket upgrade, server hello with MCP disabled,
and fresh bootstrap after automatic device reboot. This approves the
self-hosted protocol path for a later session stage while retaining mandatory
certificate trust and the existing worker/task boundaries; it does not approve
assistant UI or audio behavior.

## File and source boundaries

```text
src/app/       navigation stack
src/core/      common hardware-facing primitives and Screen base class
src/features/  UI screens grouped by user capability
src/media/     format registry, cache record helpers, project decoder adapters
src/services/  storage, network, and audio controllers
src/ui/        shared UI primitives and fonts
src/vendor_shims/  controlled compilation adapters for third_party/
```

`third_party/` is intentionally outside `src/`; Arduino compiles its required sources only through `src/vendor_shims/`. Keep project behavior in `src/` rather than editing upstream imports.
