# Plan: Lua SDK 4 — general SD file access

Status: implemented; the contract of record is [external-apps.md](../external-apps.md). Firmware builds and the example app passes a host-side mock run, but nothing here has been exercised on a device yet. Differences from the proposal below: `esp.fs.open` takes no adjacent image paths, `esp.ui.list_cell` has no trailing text, the manifest permission and read-only `/Apps` were adopted as proposed, and a covered app always stays paused (there is no fallback that stops it when the covering screen is short of memory).

Goal: an SD-card Lua app can reproduce the built-in Files screen for ordinary (non-secret) storage — browse folders, open media in the built-in viewers, create folders, rename, move, and delete.

## Where SDK 3 falls short

| Files feature | SDK 3 today | Gap |
| --- | --- | --- |
| Browse any folder | `esp.storage.*` reaches only `/.system/appdata/<id>/<name>`, flat names, 16 KiB | No path API, no directory listing |
| Sorted, paged listing | none | Sorting in Lua does not fit the 100,000-instruction callback limit beyond a few hundred entries |
| Open image/audio/video/text | none | The runtime cannot navigate, and `AppHostScreen::onExit()` calls `requestStop()`, so the app dies as soon as another screen covers it |
| Rename / new folder | none | No keyboard access from Lua |
| Delete, move | none | Recursive delete and sidecar-cache cleanup are long, multi-step jobs |
| List rows with icons | Scene rows (10, no icons) or canvas primitives | No `UiKit` list cell or file icons on the canvas; `on_touch` reports touch-down only |

Out of scope for SDK 4: secret storage, thumbnails, copying files, and reading or writing arbitrary file contents (see "Later").

## Design decisions

1. **New `esp.fs` namespace, SDK 4 only.** `esp.storage` keeps its app-data meaning. SDK 1–3 apps see no change.
2. **Manifest opt-in.** `esp.fs` is registered only when the manifest has `"sdk": 4` and `"permissions": ["files"]`. This replaces the current blanket statement that no SDK reaches the general filesystem with a per-package grant that is visible in the package.
3. **One path policy, shared with the Assistant.** Move `AssistantDeviceTools::hiddenOrPrivate` into a shared `src/services/storage/PathPolicy.h` and use it for every `esp.fs` argument and every listed entry. It rejects the secret directory, any dot-segment (`/.system`, `.cache`, `.configs`), `.tmp` names, and non-absolute paths. Additions for Lua: reject `..` and empty segments, NUL bytes, paths over 255 bytes, and internal cache formats via `FileFormats::isInternal`. Mutations are also refused under `/Apps`, so an app cannot rewrite another package's source.
4. **Host-sorted listing as a worker job.** The runtime scans a directory incrementally (same count-then-list shape as `FilesScreen::updateDirectoryScan`), keeps a PSRAM index capped at 2,000 entries, and sorts it with the Files natural comparison (extract `naturalCompare`/`compareEntries` from `FilesScreen` into a shared helper). Lua polls status and pulls pages.
5. **Mutations are one-at-a-time jobs, polled like `esp.mic`.** Each step takes `SpiBusLock` for one bounded transaction and runs from the worker loop next to `serviceRecording()`. File rename/delete run `MediaCache::beginCleanup`/`cleanupStep` first, exactly as Files does; `AppRuntime` gains a `MediaCache &` dependency.
6. **Navigation stays in the UI task.** Lua records a request; `AppHostScreen::tick()` takes it and calls `Navigator::push` with the existing `ScreenData` factories chosen through `FileFormats`. The worker never navigates.
7. **Apps survive being covered.** Split close from cover: `onExit()` pauses the runtime (no `on_tick`/`on_touch`, jobs and audio stopped or finished) instead of stopping it; `releaseResources()` still ends it; `onResume()` unpauses, redraws the last frame, and delivers `on_resume()`. This is the riskiest change — verify memory headroom with a video open over a 512 KiB Lua state, and stop the app instead of pausing if the pushed screen cannot allocate.

## Proposed API

| Operation | Contract |
| --- | --- |
| `esp.fs.scan(path)` | Start listing a folder; cancels a previous scan. Return `true` or `false, error`. |
| `esp.fs.scan_status()` | `scanning`, `count`, `truncated` (over 2,000), `path`, and `error` after a failure. |
| `esp.fs.entries(offset, count)` | Up to 16 sorted entries from the finished scan: `name`, `dir`, `size`, `modified` (epoch, 0 if unknown), `kind` (`"folder"`, `"image"`, `"audio"`, `"video"`, `"text"`, `"other"`), `openable`. |
| `esp.fs.stat(path)` | The same fields for one path, or `nil, error`. |
| `esp.fs.mkdir(path)` | Create one folder; parent must exist. `true` or `false, error`. |
| `esp.fs.rename(from, to)` | Start a rename or move job (`SD.rename`; refuses an existing target or moving a folder into itself). |
| `esp.fs.remove(path)` | Start a delete job; folders are removed recursively. |
| `esp.fs.job()` | `active`, `kind`, `done`, `total`, and `error` or `finished` for the last job. |
| `esp.fs.cancel()` | Stop the current scan or job at the next step. |
| `esp.fs.open(path [, previous, next])` | Ask the host to open a supported file in its built-in viewer; optional adjacent image paths. `true` or `false, error` (`Unsupported file`, …). |
| `esp.ui.prompt(title, initial)` | Push the keyboard; the result arrives in `on_input(text)` (`nil` when cancelled). |
| `esp.ui.list_cell(y, label, icon, trailing, action)` | `UiKit` 40 px list cell on the canvas with a kit file icon and optional 40×40 action icon. |
| `on_resume()`, `on_input(text)`, `on_touch_end(x, y, held_ms)` | New optional callbacks. `on_touch_end` enables hold-to-delete. |

Names are 1–255 UTF-8 bytes per the path policy; names are copied into Lua, never pointers. Host I/O time is excluded from the 20 ms callback limit as today.

## Work phases

1. **Foundations** — `PathPolicy` extraction (Assistant switched to it, behaviour unchanged), shared natural-sort helper, manifest `permissions` parsing in `AppPackage`, SDK 4 accepted, `MediaCache` passed into `AppRuntime` through `AppHostScreen` and `esp_media.ino`.
2. **Read-only browsing** — scan job, `scan_status`, `entries`, `stat`; canvas `list_cell` and icon ops in `AppRuntime::luaDraw` and `AppHostScreen::drawFrame`. Deliverable: example app that browses the card.
3. **Cover/resume and opening** — pause/resume lifecycle, `esp.fs.open`, `esp.ui.prompt`, `on_resume`, `on_input`. Image edge navigation currently calls `FilesScreen::imageViewerDataFor`; in SDK 4 the viewer uses only the adjacent paths the app supplied, so swiping stops after one neighbour unless that coupling is generalised (separate task).
4. **Mutations** — `mkdir`, rename/move job with cache cleanup, recursive delete job, `job`/`cancel`, `on_touch_end`. Invalidate the scan index after each successful mutation.
5. **Example and docs** — `examples/apps/files` (SDK 4, logo via `tools/generate_example_app_logos.py`); update `docs/external-apps.md` (contract, permission, revised isolation statement), `docs/architecture.md` (cover/resume, host-owned navigation requests), `docs/media.md` (apps trigger sidecar cleanup), `docs/ui-kit.md` (canvas list cell), and the README summary.

## Verification

- Build after each phase.
- On device: a folder with more than 2,000 entries, non-ASCII names, a rename and a delete of an MP3 and an image with existing `.cache` sidecars (sidecars gone, cache rebuilds), recursive delete of a nested folder, SD removal during a scan and during a delete, cancel mid-job.
- Policy: every `esp.fs` call refuses `/.secret…`, `/.system/...`, `a/../b`, `.cache`, `*.tmp`, and writes under `/Apps`; an SDK 4 app without the permission has no `esp.fs`.
- Lifecycle: open each viewer type from the app and return; open a video with the app paused; touch stays responsive during a long scan.
- SDK 1–3 examples still run unchanged.

## Decisions to confirm

- Whether the manifest permission is wanted, or SDK 4 alone should grant access.
- Whether `/Apps` should be read-only (proposed) or hidden entirely.
- Whether pause-while-covered is acceptable, or opening a file should close the app and relaunch it on return (simpler, but loses the app's folder position unless it saves it to `esp.storage`).

## Later

Chunked `esp.fs.read`/`write` for arbitrary files, file copy, thumbnails through the Files thumbnail worker, and secret-storage access through an explicit host unlock workflow.
