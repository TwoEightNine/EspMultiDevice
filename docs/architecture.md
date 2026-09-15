# Architecture and component contracts

## Runtime composition

`esp_media.ino` is the composition root. It owns pin constants, constructs each controller and screen once, registers screens with `Navigator`, starts at `Splash`, and runs a generic loop. It must not call methods on a specific screen after registration.

The one-shot USB Drive flag selects an isolated alternate composition. Transfer boot constructs only UART Serial, display, touch, SD/raw-block storage, Navigator, and `UsbDriveScreen`; it does not construct network, browser, cache, decoder, recording, or playback components. Normal boot disconnects the ROM USB-Serial/JTAG pad so the native connector remains inactive after startup. Because that register can survive a software reset, transfer boot explicitly reconnects the pad before configuring the static MSC descriptor and callbacks and calling `USB.begin()`. The flag is cleared as it is consumed, so the next reset returns to the normal composition and the native connector stops enumerating.

The loop forwards touch **state edges** only, updates SD hot-swap state, Wi-Fi, and time, then calls `tick(millis())` on the active `Screen`. The state-edge rule prevents a held press that opened a screen from also activating a control in that screen.

```text
setup: services -> Navigator -> screens -> register -> Splash
loop:  touch edge -> active Screen::handleTouch()
       SD update -> Wi-Fi update -> Time update -> active Screen::tick()
```

## Screens and navigation

All screens inherit `Screen`, which receives shared references to `Navigator`, `TouchController`, and `DisplayController`. Screen-specific services are supplied through that screen’s constructor. A screen owns its rendering, touch processing, asynchronous state, and lifecycle.

`Navigator` registers the 15 `ScreenId` values once and maintains a stack of at most eight entries. It supports `setRoot`, `push`, `replace`, and `pop`.

- `onEnter()` is a normal entry.
- `onEnter(const ScreenData&)` receives a payload; the default delegates to `onEnter()`.
- `onExit()` runs when leaving the active screen.
- `onResume()` runs when a stacked screen becomes active again; its default redraws through `onEnter()`.
- `onResult(const ScreenResult&)` receives a result from `pop(result)`.
- `handleTouch()` receives a changed `TouchPoint`; `tick()` must stay non-blocking.

Use `ScreenData` for image, audio, video, text-input, and secret-files flows. Image payloads can carry adjacent image paths for edge navigation and the full-screen state to retain during an adjacent-image replacement; image zoom never transfers. `ScreenResult::withText()` returns keyboard text to the caller. Do not introduce direct cross-screen calls for new navigation flows.

## Core services

| Component | Responsibility and contract |
| --- | --- |
| `DisplayController` | Initializes TFT_eSPI, exposes the display, reports dimensions, and clears the panel. |
| `TouchController` | Polls XPT2046 over SPI; debounces press/release, filters samples, maps to the display, and persists 4×4 mesh calibration at `/.system/touch`. |
| `SpiBusLock` | FreeRTOS mutex for the SPI wires shared by SD and XPT2046. Concurrent work locks only individual transactions. |
| `BatteryController` | Reads a divider-corrected cell voltage from the median of three calibrated samples. During one controller session, it retains the lowest median voltage; each returned voltage and its linearly interpolated discharge-curve percentage therefore never increase. The percentage is calculated from exactly the voltage returned in the same `BatteryReading`. |
| `SdCardController` | Mounts, monitors, and remounts FAT storage. `update()` is called from the main loop and consumers obtain pending `Mounted`, `Removed`, or `MountFailed` state through `takeEvent()`. |
| `UsbMassStorageController` | Owns the TinyUSB MSC callbacks and host-visible state. In transfer boot it asks `SdCardController` to replace local FAT ownership with an unmounted raw SPI block device, exposes the whole card, and reports eject/disconnect/error state to `UsbDriveScreen::tick()`. |
| `MediaCache` | Owns source-derived cache paths and fingerprints, bounded SPI-safe cache file operations, `.tmp` write transactions, atomic publication, abort cleanup, incremental sidecar cleanup, and staged sidecar relocation across encryption boundaries. Cache codecs retain their format-specific header and payload validation. |

Without a card-detect pin, `SdCardController` probes an attached card every 500 ms and retries an absent card every three seconds. With card detect, mount attempts occur after the physical state changes. SD screens must react to events from their `tick()` methods, not assume a mounted card remains available.

## Concurrency and memory

The UI loop must continue to process touch and redraw while expensive work is under way. Files thumbnails, browser inline images, and animated-image cache loading use worker work on the other core; results are returned to the UI task for display updates. Do not let a worker redraw UI directly.

SD and touch share clock/data lines. Long media reads are deliberately chunked, acquire `SpiBusLock` only around a read, and release it between chunks. Media decoders and frame rings rely on PSRAM. The supported N16R8 profile is not optional for the full animated-image/video feature set.

USB MSC callbacks run outside the UI task. They never draw, navigate, or restart. Each 512-byte SD sector transaction takes `SpiBusLock` and releases it before the next sector so touch polling can proceed. Safe eject, disconnect, and I/O failures are published as state and acted on by the USB Drive screen. Local FatFs is fully unmounted before raw media becomes present to the host; normal and host filesystem ownership never overlap.

`MediaCache` is constructed once in the composition root and shared by Files, Image Viewer, Audio Player, and Video Player. Its reader, writer, cleanup, and relocation operations report busy separately from failure. Screen work retries incremental cleanup or relocation from `tick()`, and cache workers yield between bounded transfers; the service never decodes media, paints, or navigates.

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
is its sole user. Download and session-page-cache operations take the shared
SPI lock only for bounded 4 KiB SD transactions, release it, and yield.
`BrowserTransport::closeWhenIdle()` is UI-safe: it closes an idle connection
immediately or asks the serialized worker to close after cancellation, so the
UI never reaches into a live client object.

`BrowserSessionCache` owns the Browser session's bounded page/image index,
LRU accounting, and cache mutex. Its disposable files remain under
`/.system/browser_session/`; the Browser lifecycle decides when to begin or
end a session, while cache state is never duplicated in the screen.

`BrowserLifecycleWorker` owns the Browser's one-entry lifecycle queue, worker
task, generation invalidation, and completion signal. It invokes a narrow
Browser callback for cache cleanup and persistent-state loading; the callback
never draws, while `BrowserScreen::tick()` consumes the completion signal and
redraws on the UI task.

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

## Assistant dependency boundary

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

Stage 1 adds a compatibility diagnostic, not assistant behavior.
`XiaozhiIdentity` owns the assistant's stable identifiers: the device ID is the
lowercase Wi-Fi station MAC and a random UUIDv4 is generated once in NVS under
`assistant/client_uuid`. A malformed stored UUID is a hard error because
silently replacing it would change the server-side device identity.
`XiaozhiBootstrapClient` owns the certificate-checked HTTP POST, upstream-style
board report, bounded response, and parsing of only activation and WebSocket
configuration. Its endpoint and PEM trust anchor can be supplied by a
build-local configuration while the official hosted endpoint remains the
default. It rejects non-HTTPS bootstrap configuration, never exposes firmware
or MQTT response data, and never logs the raw response or credentials.

The Stage 1 probe is selected only by
`ESP_MEDIA_XIAOZHI_COMPATIBILITY_PROBE`. It runs in a dedicated 48 KiB task,
uses the existing secure WebSocket adapter, sends one protocol `hello`, waits for a
validated server `hello`, and disconnects. No audio, MCP request, system
command, or other server message is acted on. The normal composition root does
not instantiate either Stage 1 service.

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
