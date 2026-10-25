# Plan: built-in screens as Lua system apps

Status: proposal, nothing implemented. The SDK contract of record stays [external-apps.md](../external-apps.md); this plan names what the SDK must gain so that every user-facing screen, Home included, can be a Lua app while core work stays in C++.

## Rule for the split

Lua owns **composition and policy**: layout, which controls exist, what a tap means, state machines measured in hundreds of milliseconds, text for labels, and which file or service to call. C++ owns everything that is **per-pixel, per-sample, per-byte, per-socket, sub-100 ms, or a security boundary**: decoders, caches, DMA/I2S, TLS, HTML parsing, text wrapping of whole files, swipe physics, frame presentation, sorting large listings, encryption, credential storage, and the USB composition switch.

Today's runtime numbers this has to fit: 100,000 instructions and 20 ms per callback, a 100 ms `on_tick`, a 512 KiB PSRAM Lua heap, one 16 KiB worker stack on core 0, frames of 64 commands that the host redraws in full (clear canvas, replay, redraw app bar) on every `present()`, and touch delivered only as down/up edges.

## Screen inventory

`ScreenId` has 22 entries. This lists what each does, who opens it, the C++ it depends on, and the verdict.

| Screen | Use cases | Opened by / payload | Heavy C++ it relies on | Verdict |
| --- | --- | --- | --- | --- |
| **Splash** | 3 s logo, Wi-Fi start, then `BatteryHistoryService::begin`, `replace(Home)` | boot root | logo blit | **Stays native.** Runs before any Lua state exists. |
| **Home** | status bar (clock, SD, Wi-Fi, battery voltage); built-in plus `/Apps` tiles sorted by name; 9-tile pages; drag paging with 48 px / 650 px/s flick, rubber band, cubic settle; PSRAM sprite strip with per-slot invalidation; hold Files for 1 s → PIN prompt → secret Files; catalog rescan on SD remount | root after Splash; `onResult` PIN text | `AppCatalogLoader` worker (logo PNG decode), 320×437 sprite strip, 16 ms animation | **Lua app over a native launcher pager widget.** Gesture/animation cannot run at a 100 ms tick through full-frame redraws. |
| **Files** | paged listing (folders first, natural sort), 8-directory LRU, open by format, rename (keyboard), new folder, move (choose destination), hold-to-delete with progress, recursive delete, sidecar cache cleanup/relocation, thumbnails (12 LRU, worker), secret mode (encrypted names, Move to/out of secret via copy + crypt + staging), SD hot-swap, image neighbours for viewer edge taps | Home tile; Home PIN; Assistant `OpenFolder` (normal folder) | scan/sort, `FileThumbnailWorker`, `MediaCache` cleanup/relocation, `SecretStorage` copy jobs (2,527 LOC) | **Lua app.** `examples/apps/files` already covers ordinary storage; gaps are thumbnails, copy, secret storage, and viewer neighbours. |
| **Image Viewer** | aspect fit, zoom, animated GIF/WebP, bar/fullscreen toggle, edge taps to neighbours, secret images | Files, Assistant `OpenImage` | `ImageView` (2,856 LOC) | **Lua app** — SDK 5 `examples/apps/images` already does it; needs launch payloads and secret handles. |
| **Audio Player** | WAV/MP3 play/pause, MP3 analysis + duration cache, progress/time, volume picker, secret files, SD loss | Files, Assistant `LaunchAudio` | minimp3, WAV parser, `.empa` cache, streaming loop (1,005 LOC) | **Lua shell over a headless audio-playback service** extracted from the screen. |
| **Video Player** | MP4/3GP H.264/H.263/MPEG-4 + AAC/MP3/AMR, fullscreen/embedded, landscape rotation, play-control underlay restore, auto-fullscreen after 2 s, secret files | Files, Assistant `PlayVideo` | `VideoStreamWorker` on core 1, frame slots, TFT+touch rotation (2,177 LOC) | **Stays a native viewer** reached through the intent registry. Its chrome is frame-synchronised; a Lua shell adds risk and no flexibility. Revisit after the layer framework exists. |
| **Text Viewer** | read-only paging of `.txt/.json/.lua/.config`, 512 B reads per tick, 64 page markers, rescan for previous pages, page/byte progress, secret files | Files | UTF-8 glyph wrapping and paging (685 LOC) | **Lua shell over a native text-pager layer.** |
| **Text Input** | QWERTY/symbols/shift/caps, cursor placement, multiline wrap with scroll, masked text, 4-digit PIN mode, returns `ScreenResult::withText` | Files, Wi-Fi, Browser, Home PIN, apps via `esp.ui.prompt` | key layout, blink, 120 ms key feedback | **Stays native** as the system keyboard. `esp.ui.prompt` gains options. |
| **Voice Recorder** | record to `/voice_last.wav`, stop, play, mic power switch, storage/detail lines | Home tile | mic/speaker/WAV in `AppRuntime` already | **Lua app** — `examples/apps/recorder` exists; needs writing outside app data (or keep app-data path). |
| **Workout** | interval setup steppers, stages, tone cues, pause, total time, volume | Home tile | tone synth already in `AppRuntime` | **Lua app** — `examples/apps/workout` exists; needs partial redraw so the timer does not repaint the canvas every second. |
| **Settings** | 7-row list: Wi-Fi, Speed test, Audio, LED, Battery, Touch screen, About (native: logo, repository, firmware version) | Home tile | none | **Lua app**, trivial once apps can launch apps. |
| **Audio Settings** | 0–10 volume grid | Settings | `AudioSettings` | **Lua app** — API exists. |
| **LED** | enable toggle, palette swatches, unavailable error | Settings | `LedSettings` | **Lua app** + small `esp.led`. |
| **Battery Info** | raw/smooth voltage, chart of up to 256 history points with grid | Settings, Assistant `OpenBatteryDetails` | `BatteryHistoryService` | **Lua app** + `esp.battery` + polyline draw op. |
| **Wi-Fi** | enable switch, scan, network list (3 visible), connect with password prompt, connected view, forget, rescan, loader | Settings | `WiFiController` (NVS credentials) | **Lua app** + `esp.wifi` (system permission). |
| **Speed Test** | Wi-Fi (Cloudflare TLS) and SD (verified read/write) modes, progress, metrics, cancellable close | Settings | two opposite-core workers | **Lua app** + `esp.bench` snapshots; workers stay. |
| **Touch Calibration** | menu, draw test, 4×4 mesh calibration with targets, persists to `/.system/touch` | Settings | raw touch samples, mesh math | **Stays native.** It is the recovery path when touch is wrong, and it needs raw continuous samples. Launchable by intent. |
| **Browser** | start page (16 recents/favorites, paging, add URL), HTTP(S) load with progress/cancel, HTML/CSS rows, pagination, links, forms (8/32/80), inline image previews, history with anchors, UA toggle, favorites, downloads, session cache, Assistant web search | Home tile, Assistant `SearchWeb` | transport, document, page loader, image worker, persistence (~7,000 LOC) | **Stays native in this programme.** Optional later: Lua start page over a native web-document layer. |
| **USB Drive** | intro + request transfer boot + restart (normal boot); MSC state/eject/restart (transfer boot) | Home tile; transfer-boot root | `UsbDriveBoot`, `UsbMassStorageController` | **Split.** Normal-boot intro becomes Lua with a privileged `esp.system.usb_drive()`; transfer-boot screen stays native (that composition builds no Lua). |
| **Mac Remote** | 3×3 HID actions (YouTube macro, screenshot, lock, media keys, volume), connection state, busy, release on exit | Home tile | `MacRemoteController` (TinyUSB HID) | **Lua app** + `esp.hid` + kit icons. |
| **Assistant** | 320×432 background, 320×240 avatar frames composited over it, user/assistant captions (CJK font, 3-line paging every 2 s), voice control (tap, 800 ms hold → auto-listen, cyan pulse), overflow menu (new conversation two-tap, avatar sets), X-only close handshake, stays alive under screens it opened (`tickSuspended`), launches media/folder/battery/web after the turn reaches Ready | Home tile | `AssistantController`, `XiaozhiSession`, avatar loader, caption layout | **Lua shell, last.** Needs an avatar/background layer, caption widget, `esp.assistant`, and a host-gated launch path. |
| **ExternalApp** | hosts one SD Lua app | Home, payload `ExternalApp` | `AppRuntime` | Becomes the **universal Lua host** for system and SD apps. |

Result: 15 screens become Lua (Home, Files, Image Viewer, Audio Player shell, Text Viewer shell, Recorder, Workout, Settings, Audio, LED, Battery, Wi-Fi, Speed Test, Mac Remote, USB Drive intro), Assistant follows last, and 5 stay native (Splash, Text Input, Touch Calibration, Video Player, Browser) plus the transfer-boot USB screen and a new safe-mode launcher.

## Platform gaps that block any of this

These are runtime changes, independent of individual APIs, and come first.

1. **Apps in firmware, not only on SD.** Home, Settings, and Wi-Fi cannot disappear when the card is pulled, and today SD removal ends every app. System packages (manifest, Lua sources, logo) are generated into flash by a new `tools/generate_system_apps.py` (from `apps/system/<id>/`), merged into the catalog, and survive SD removal. Their IDs are reserved; an SD package with the same ID is ignored. System apps keep settings in a new NVS-backed `esp.prefs`, because `esp.storage` lives on SD.
2. **More than one Lua app alive.** Home → Files → Images is three live states. `Navigator` maps one `Screen` per `ScreenId`, and `AppHostScreen` owns one `AppRuntime`. Replace the single host with a pool (or let Navigator stack dynamic app entries) and replace one-task-per-app with **one Lua scheduler task** that runs callbacks only for the active state; covered states stay frozen in PSRAM. A task per app would cost 16 KiB of internal RAM each, which the TLS and Wi-Fi paths cannot spare. Add a global Lua heap budget (for example 2 MiB across the stack) and a per-manifest limit.
3. **Launch, payloads, and results instead of `ScreenData`.** Apps open apps and native viewers by ID or by file, pass bounded arguments, and return a result. The host maps Assistant launch events and file opens onto the same intents, so neither the worker nor the backend navigates.
4. **Rendering that can update part of the screen.** Full clear + replay + app bar on each present flickers and costs a full 320×432 fill for a one-digit timer change. The host needs a PSRAM back buffer, dirty-rect presentation in one TFT transfer (as Home already does), a larger command budget, and a no-chrome mode for Home, Assistant, and full-screen layers.
5. **Continuous touch.** Moves are not forwarded to any screen (Home polls `TouchController::point()` in `tick()`). The host must sample held positions and deliver coalesced moves, plus a hold event that fires while the finger is still down (Home's 1 s Files hold opens the PIN prompt before release).
6. **Trusted versus SD permissions.** Wi-Fi credentials, secret storage, USB reboot, HID, and the Assistant must not be reachable by any SD package that writes the right manifest string.
7. **Failure fallback.** If the system Home fails to load or errors, the device needs a native safe-mode launcher (list of system apps plus Touch Calibration), not a dead screen.

## SDK modules, size, and depth

Size is the C++ work: **S** under ~300 lines, **M** 300–1,000, **L** 1,000–2,500, **XL** more, counting code that moves out of a screen. Depth says how much of the subsystem Lua sees.

### Runtime and host

| Module | Depth | Contents | Size | Needed by |
| --- | --- | --- | --- | --- |
| System packages | build-time | Generator, flash catalog, reserved IDs, SD survival, `require` of package-local modules and one shared `sys/ui.lua` helper library (the 64 KiB single-file limit will not hold Files), larger source limit for flash apps | M | all |
| App stack and scheduler | invisible | Host pool or dynamic entries, one scheduler task, frozen covered states, global and per-app heap budgets, `on_pause`/`on_resume` for every cover (not only SDK 4 navigation) | L | all |
| Intents `esp.app` | full | `launch(id, args)`, `open(path [, context])` resolved through `FileFormats` to a handler (Lua or native), `finish([result])`, `on_start(args)`, `on_result(from, result)`, `list()` for the launcher; args are bounded tables (strings, numbers, booleans, depth 2, about 2 KiB) | M | Home, Files, Settings, every viewer |
| `esp.prefs` | full | NVS key/value, bounded keys and values, namespaced per app | S | system apps |
| Lifecycle events | full | `on_sd(state)` for mounted/removed/failed, `esp.app.set_tick(ms)` down to 33 ms for the active app | S | Home, Files, Recorder, players |
| Safe mode | none | Native fallback launcher when a system app cannot start | S | Home |
| Simulator | tooling | Desktop Lua harness with a stub `esp` and PNG frame dumps, so system apps are tested without flashing | M | all |

### Drawing and input

| Module | Depth | Contents | Size |
| --- | --- | --- | --- |
| Render v2 | primitives | PSRAM back buffer shared by the active app, `begin_update(x, y, w, h)` partial frames composited and pushed as one rect, command budget about 512 in PSRAM, `chrome = "bar" \| "none"`, app-bar options (back or close, title, trailing More/Zoom), clipping | L |
| Draw ops v2 | primitives | `polyline` (≤256 points), `icon(name, …)` over a named registry of `icons.h`/`UiKit` glyphs (nav, playback, keyboard, remote, voice, mic, favorite, zoom, page arrows), `asset(handle, x, y)` for package PNGs decoded by the host, text with max width/ellipsis and wrapped boxes through `UiText`, toggle, swatch, popup menu frame/row, battery indicator | M |
| Input v2 | events | `on_touch_move(x, y)` coalesced to the frame rate, `on_touch_hold(x, y, ms)` at an app-set threshold, raw-coordinate opt-out for full-screen layers | S |
| Launcher pager widget | widget, Lua supplies data | Home's tile grid, sprite strip, swipe physics, page dots, per-slot invalidation, status bar (clock/SD/Wi-Fi/battery) as host components; Lua supplies tiles `{id, label, icon_handle, color}` and gets `on_action(id)` and `on_hold(id)` | L (mostly moved from `HomeScreen`) |
| Keyboard options | parameters | `esp.ui.prompt(title, {initial, mask, mode = "text" \| "pin4"})` | S |

### Media layers

These generalise the SDK 5 pattern: Lua records the desired layer state, the UI task reconciles it, and the worker receives a status snapshot. Pixels and samples never cross into Lua.

| Module | Depth | Contents | Size |
| --- | --- | --- | --- |
| Layer framework | invisible | Refactor `esp.image` handling into a common host-layer interface (rect, chrome painter, touch routing, status serial) so new layers do not each copy `AppHostScreen` code | M |
| `esp.image` additions | small | Open with a secret handle; keep neighbour handling in Lua | S |
| `esp.player` (audio) | state and commands | Extract WAV/MP3 decoding, MP3 analysis, and `.empa` cache from `AudioPlayerScreen` into an `AudioPlaybackService` for arbitrary SD paths (today `esp.audio.play_file` reads app-data WAV only): `open`, `play`, `pause`, `stop`, `status{state, elapsed_ms, duration_ms, format, error}` | L |
| `esp.textview` | state and commands | Extract the text pager into a layer bound to a canvas rect: `open(path)`, `next`, `previous`, `status{state, page, percent, error}` | M |
| `esp.video` | none for now | Video stays a native viewer reached by `esp.app.open` | — |
| Avatar layer | state | Background PNG plus avatar frame compositing for Assistant: `set_avatar_set(dir)`, `set_state(name)` | M (moved) |

### Services

| Module | Depth | Contents | Size | Permission |
| --- | --- | --- | --- | --- |
| `esp.fs` additions | jobs | Copy job, `open(path, {neighbours from current listing})`, thumbnails (`thumbnail(path)` returns a handle drawn by `list_cell`; `FileThumbnailWorker` moves out of Files), write outside app data for Recorder | M–L | `files` |
| `esp.secret` | jobs and handles | `prompt_unlock()` shows the native PIN keyboard and returns an opaque session handle, so the PIN never enters Lua; secret-scoped scan/stat/rename/remove with host-decrypted names; move-to/move-out jobs (copy + XOR + staging + cache relocation, moved from Files) | L | system only |
| `esp.apps` catalog | snapshot | Move `AppCatalogLoader` into a service: scan status, entries with logo handles, rescan on SD remount | M (moved) | system only (launcher) |
| `esp.time` | snapshot | Local time table, validity, formatted `HH:MM` | S | none |
| `esp.battery` | snapshot | Latest raw/smooth voltage, generation, `history()` of up to 256 points | S | none |
| `esp.led` | settings | available, enabled, color, palette names/RGB565, `set_enabled`, `set_color` | S | system only |
| `esp.wifi` | settings and jobs | enabled, state, ssid, type, error, `scan`, `networks`, `connect(ssid, password)`, `forget`, `set_enabled` | S–M | system only |
| `esp.bench` | jobs | Wi-Fi and SD speed-test start, cancel, snapshot; workers unchanged | S | system only |
| `esp.hid` | commands | `begin`, `state`, `busy`, `send(action)`; host releases all keys on pause/exit | S | system only |
| `esp.system` | commands | `info{version, heap, psram, sdk}` (About), `usb_drive()` request + restart, `open_native("touch_calibration")` | S | system only |
| `esp.assistant` | snapshot and commands | Phase, latest caption pair, mic powered, auto-listen; `start_turn`, `cancel`, `set_auto_listen`, `new_conversation`, `retry`; activation and safe deactivation tied to app lifetime; host keeps `tickSuspended` behaviour and gates launches until the turn is Ready, then performs them as intents | M–L | system only |

## What stays native and why

- **Splash** — runs before the Lua stack; starts battery sampling.
- **Text Input** — shared system component used by every app; becomes a parameterised prompt.
- **Touch Calibration** — recovery path; needs raw samples; must work even if Lua UI is broken.
- **Video Player** — frame-synchronised chrome, rotation, opposite-core worker; reached by intent.
- **Browser** — about 7,000 lines of document/transport/worker code, where only the start page would benefit; revisit as a Lua start page over a native document layer after everything else.
- **USB Drive transfer boot** — isolated composition with no Lua.
- **Safe-mode launcher** — new, minimal fallback.

## Phases

1. **Platform** — system packages in flash, scheduler and app stack, pause/resume on every cover, intents, `esp.prefs`, `on_sd`, safe mode, simulator. Deliverable: Settings, Audio Settings, and LED as system apps launched from the native Home. Measure internal heap and PSRAM with Home + Files + Images + a native viewer stacked.
2. **Rendering and input v2** — back buffer, partial frames, larger budget, chrome modes, icons, assets, polyline, moves and holds. Deliverable: Workout and Recorder promoted from examples to system apps, Battery Info, Mac Remote, USB Drive intro.
3. **Settings services** — `esp.wifi`, `esp.bench`, `esp.time`, `esp.battery`, `esp.led`, `esp.hid`, `esp.system`. Deliverable: Wi-Fi and Speed Test apps; the native Settings family is deleted.
4. **Media layers** — layer framework, `esp.player`, `esp.textview`, image secret handles. Deliverable: Images, Audio Player, and Text Viewer apps; video remains native behind the same intent.
5. **Files parity** — copy job, thumbnails, viewer neighbours, `esp.secret` and secret move jobs. Deliverable: Files app with full parity; native `FilesScreen` deleted.
6. **Home** — `esp.apps` catalog, launcher pager widget, status bar, hold → PIN → secret Files flow. Deliverable: Lua Home; native Home kept only as safe mode.
7. **Assistant** — avatar layer, caption widget, `esp.assistant`, host-gated launches through intents (check against [agentic-commands.md](../agentic-commands.md): launches must stay controller-owned UI actions).
8. **Optional** — Browser start page over a native web-document layer; Lua video chrome.

Each phase updates `external-apps.md` (new SDK version), `architecture.md` (host, scheduler, layers), `ui-kit.md` (canvas components), the focused service docs, and the README summary, and is built with `tools/build_esp32s3.sh`.

## Risks and measurements to take first

- **Internal RAM.** Several frozen Lua states plus a video worker plus TLS. Measure with Navigator's release audit before committing to the stack depth; cap live Lua apps (for example four) and stop the oldest covered app when the cap is reached.
- **Lua speed on device.** Benchmark a 1,000-entry listing page, a 256-point chart, and a 9-tile redraw against the 100,000-instruction / 20 ms budget before choosing what moves into widgets.
- **Flash size.** System Lua sources are small, but moved code plus a back buffer path must stay inside the 6,553,600-byte slot that `build_esp32s3.sh` enforces.
- **Feature drift.** Several examples are reduced rebuilds (Files has no thumbnails or secret storage). Delete a native screen only after its use-case row above passes on the device.
- **Security wording.** System-only permissions stop an SD package from requesting sensitive services; they are still not hostile-code isolation, and `SecretStorage` stays obfuscation, as documented today.
