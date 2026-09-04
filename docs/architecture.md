# Architecture and component contracts

## Runtime composition

`esp_media.ino` is the composition root. It owns pin constants, constructs each controller and screen once, registers screens with `Navigator`, starts at `Splash`, and runs a generic loop. It must not call methods on a specific screen after registration.

The loop forwards touch **state edges** only, updates SD hot-swap state, Wi-Fi, and time, then calls `tick(millis())` on the active `Screen`. The state-edge rule prevents a held press that opened a screen from also activating a control in that screen.

```text
setup: services -> Navigator -> screens -> register -> Splash
loop:  touch edge -> active Screen::handleTouch()
       SD update -> Wi-Fi update -> Time update -> active Screen::tick()
```

## Screens and navigation

All screens inherit `Screen`, which receives shared references to `Navigator`, `TouchController`, and `DisplayController`. Screen-specific services are supplied through that screen’s constructor. A screen owns its rendering, touch processing, asynchronous state, and lifecycle.

`Navigator` registers the 14 `ScreenId` values once and maintains a stack of at most eight entries. It supports `setRoot`, `push`, `replace`, and `pop`.

- `onEnter()` is a normal entry.
- `onEnter(const ScreenData&)` receives a payload; the default delegates to `onEnter()`.
- `onExit()` runs when leaving the active screen.
- `onResume()` runs when a stacked screen becomes active again; its default redraws through `onEnter()`.
- `onResult(const ScreenResult&)` receives a result from `pop(result)`.
- `handleTouch()` receives a changed `TouchPoint`; `tick()` must stay non-blocking.

Use `ScreenData` for image, audio, video, text-input, and secret-files flows. Image payloads can carry adjacent image paths for edge navigation. `ScreenResult::withText()` returns keyboard text to the caller. Do not introduce direct cross-screen calls for new navigation flows.

## Core services

| Component | Responsibility and contract |
| --- | --- |
| `DisplayController` | Initializes TFT_eSPI, exposes the display, reports dimensions, and clears the panel. |
| `TouchController` | Polls XPT2046 over SPI; debounces press/release, filters samples, maps to the display, and persists 4×4 mesh calibration at `/.system/touch`. |
| `SpiBusLock` | FreeRTOS mutex for the SPI wires shared by SD and XPT2046. Concurrent work locks only individual transactions. |
| `BatteryController` | Reads a divider-corrected cell voltage; reports the median of nine calibrated samples after ~15-second smoothing and one-percent hysteresis. |
| `SdCardController` | Mounts, monitors, and remounts FAT storage. `update()` is called from the main loop and consumers obtain pending `Mounted`, `Removed`, or `MountFailed` state through `takeEvent()`. |

Without a card-detect pin, `SdCardController` probes an attached card every 500 ms and retries an absent card every three seconds. With card detect, mount attempts occur after the physical state changes. SD screens must react to events from their `tick()` methods, not assume a mounted card remains available.

## Concurrency and memory

The UI loop must continue to process touch and redraw while expensive work is under way. Files thumbnails, browser inline images, and animated-image cache loading use worker work on the other core; results are returned to the UI task for display updates. Do not let a worker redraw UI directly.

SD and touch share clock/data lines. Long media reads are deliberately chunked, acquire `SpiBusLock` only around a read, and release it between chunks. Media decoders and frame rings rely on PSRAM. The supported N16R8 profile is not optional for the full animated-image/video feature set.

## Browser internal boundaries

`BrowserScreen` remains the Browser screen's Navigator and UI-task coordinator.
Its browser-internal records and limits live in `BrowserTypes.h` and
`BrowserLimits.h`, so bounded contracts are shared rather than duplicated in a
screen declaration.

`BrowserTransport` owns the HTTP client pair and their serialization mutex. It
has no Navigator, touch, display, or SD dependency. `BrowserPersistentState`
owns recents, favorites, cookies, and the plaintext browser-state file; it has
no UI dependency. Both return data or terminal results to `BrowserScreen`,
preserving the rule that no worker or service paints the display or navigates.

`BrowserSessionCache` owns the Browser session's bounded page/image index,
LRU accounting, and cache mutex. Its disposable files remain under
`/.system/browser_session/`; the Browser lifecycle decides when to begin or
end a session, while cache state is never duplicated in the screen.

`BrowserLifecycleWorker` owns the Browser's one-entry lifecycle queue, worker
task, generation invalidation, and completion signal. It invokes a narrow
Browser callback for cache cleanup and persistent-state loading; the callback
never draws, while `BrowserScreen::tick()` consumes the completion signal and
redraws on the UI task.

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
