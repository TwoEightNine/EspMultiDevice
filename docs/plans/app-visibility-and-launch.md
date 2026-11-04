# Plan: Home visibility, per-device enabling, and launching apps by ID

Status: implemented; the SDK contract of record is [external-apps.md](../external-apps.md). The firmware builds and `tests/app_disabled_list` passes on the host; the example apps were run against a stubbed `esp` table on host Lua. Nothing has been run on a device yet, so the device checks under Verification are still open. Differences from the proposal below:

- The catalog worker reads `AppSettings` directly (it is mutex-guarded) instead of taking a snapshot from Home.
- Settings → Apps shows nine rows per page above a 42px pager, and marks hidden apps *Not in Home*. The row is the new `UiKit::drawToggleListCell` with a 56×28 toggle.
- `installed` is false only when the manifest is missing; a manifest that exists but cannot be read counts as an installed, invalid package.
- No host test runs `esp.apps` itself: the Lua host harnesses do not compile `AppRuntime`.

Goal: every SD app has two properties.

| Property | Owner | Meaning |
| --- | --- | --- |
| `show_in_home` | package manifest | Home shows a tile for the app. When false, only another app can start it. |
| enabled | this device (Settings → Apps) | The app may run at all. A disabled app has no Home tile, cannot be launched by another app, and refuses to start. |

SDK 7 adds `esp.apps.status(id)` and `esp.apps.open(id)` so apps can check and launch other apps by ID.

## 1. Manifest field `show_in_home`

- Optional boolean, default `true`, accepted for every SDK version (it is launcher metadata, not Lua API). Older firmware ignores it and shows the tile, which is harmless.
- A non-boolean value makes the package invalid with `Invalid show_in_home`, like an invalid `hosts` array.
- `AppPackage` gains `bool showInHome = true`, parsed in `AppPackage::load`.
- `show_in_home` is not checked at launch: an app started by `esp.apps.open` may have either value.

## 2. Device setting "enabled"

### Storage: `AppSettings` service

New `src/services/apps/AppSettings.{h,cpp}`, built by the composition root like `LedSettings`.

- ESP32 Preferences namespace `apps`, key `disabled`: one blob of `\n`-separated app IDs. Only disabled IDs are stored, so a newly installed app is enabled.
- At most 64 IDs (≤ 2,112 bytes). When full, disabling another app drops the oldest entry, which re-enables that (probably long-removed) app.
- NVS rather than SD: the setting belongs to this device. Moving a card to another device does not carry it, and swapping cards does not lose it.
- `isEnabled(id)` and `setEnabled(id, bool)` take an internal mutex because the app worker (core 0) reads while the Settings UI writes. `setEnabled` reports success only after the blob is committed.
- Nothing is deleted from `/.system/appdata/<id>` when an app is disabled.

### Enforcement

| Place | Rule |
| --- | --- |
| `AppCatalogLoader` (Home mode) | Skips packages with `show_in_home == false` or disabled before they use one of the 32 slots, and does not decode their logos. Home passes a snapshot of the disabled list with `requestScan()` so the worker never reads UI state. |
| `AppHostScreen::startApp()` | Refuses a disabled app with an *App disabled* page. Covers Home, `esp.apps.open`, and any future launch path. |
| `esp.apps.open` | Returns `false, "App disabled"`. |

### Settings → Apps

- New `AppsSettingsScreen` (`ScreenId::AppsSettings`), opened from a new 8th Settings row *Apps* (8 × 40 px rows end at y = 384).
- `AppCatalogLoader` gains an `All` scan mode: every valid package, no logos, with `showInHome` and `enabled` in `Entry`. The screen owns its own loader instance and rescans on SD remount.
- One `UiKit::drawListCell` per app with `UiKit::drawToggle`; the whole 40 px row toggles. Ten rows per page with paging like Files. Apps with `show_in_home == false` carry a compact *Opened by other apps* note; if UiKit has no subtitle cell, add one to `UiKit` and document it in [ui-kit.md](../ui-kit.md).
- Empty state: *No apps on SD card* / *SD card unavailable*.
- `Navigator::MaxScreens` goes from 23 to 24: `ScreenId` already has exactly 23 entries.

## 3. SDK 7: `esp.apps`

SDK 7 keeps every SDK 6 operation. `esp.apps` exists for every SDK 7 package and needs no permission: it reveals only what the caller asks about by exact ID and cannot enumerate installed apps.

| Operation | Contract |
| --- | --- |
| `esp.apps.status(id)` | Return `{installed, valid, enabled, show_in_home, launchable, name, sdk, error}` for an ID matching `[a-z][a-z0-9_-]{0,31}`. `installed` is true when `/Apps/<id>/manifest.json` exists; `valid` when the package passes the same checks as Home; `name`, `sdk`, and `show_in_home` are present only when valid; `error` is the package error when installed but invalid; `enabled` reads `AppSettings` (true for unknown IDs); `launchable` is `valid and enabled and id ~= own id`. Return `nil, error` for `Invalid app ID`, `SD card unavailable`, or `SD busy`. |
| `esp.apps.open(id)` | Ask the host to replace the running app with app `id`. Return `true` once accepted, or `false, error`: `Invalid app ID`, `App not found`, `App invalid`, `App disabled`, `Already running` (own ID), `Navigation pending`, `SD card unavailable`, `SD busy`. |

Semantics of `open`:

- Validation runs inside the callback through `AppPackage::load` plus `AppSettings::isEnabled`. As with `esp.storage`, the bounded manifest read and its bus wait (≤ 250 ms) are host work outside the 20 ms limit.
- The launch is a **replace**: after the callback returns, the caller finalizes any recording, stops tones and playback, closes its image layer, cancels network requests, and ends; the target then starts with a fresh Lua state. Closing the target returns to whatever was below the caller (normally Home), not to the caller. The target has no way to tell who launched it.
- Return-to-caller (push) needs more than one live Lua state, which is gap 2 in [lua-system-apps.md](lua-system-apps.md). When that lands, add `esp.apps.push(id)` (or an option) rather than changing `open`.
- Launch arguments and results are out of scope; the apps can exchange data only through their own storage today, and cross-app data is not shared.

### Implementation

- `AppPackage::load`: accept `sdk` 7. `AppRuntime::start`: register `esp.apps` when `_sdkVersion >= 7` (new `AppRuntimeSdk7.cpp` with `registerSdk7()`, following `registerSdk6()`).
- `AppRuntime` takes an `AppSettings &` (constructor, via `AppHostScreen`).
- `NavigationKind::LaunchApp` with `char appId[33]` in `Navigation`; `esp.apps.open` fills it through the existing `_navigationPending` hand-off, so the worker never navigates.
- `AppHostScreen::openNavigation()` handles `LaunchApp` by setting a pending target and reusing the restart path in `tick()`: `requestStop()`, wait for `stopped()` and `end()` (which already waits for the network task), then `_appId = target`, `resetHostState()`, `startApp()`. The `ExternalApp` stack entry is reused in place; its payload is not read after `onEnter`.
- If the target fails to start (removed or disabled between check and start), the host shows the usual *Cannot start app* / *App disabled* page for the target.

## 4. Examples and tests

- `examples/apps/launcher` (SDK 7, shown in Home) and `examples/apps/launcher_helper` (SDK 7 or lower, `"show_in_home": false`). The launcher shows `esp.apps.status` for the helper and an unknown ID, and opens the helper.
- `tests/`: a host test for `esp.apps.status`/`open` argument validation and error strings, in the style of `tests/lua_net_contract`, if that harness can host `AppRuntime` without SD; otherwise device checks only.

## 5. Docs

- [external-apps.md](../external-apps.md): title and SDK range to 7, `show_in_home` in *Package contract*, a section on enabling, and an *SDK 7 apps* section with the table above.
- [services.md](../services.md): `AppSettings` NVS format and limits.
- [architecture.md](../architecture.md): `AppsSettingsScreen`, the `LaunchApp` hand-off, `MaxScreens`.
- README summary and CHANGELOG.

## Verification

- Firmware builds.
- Device: a `show_in_home: false` app has no Home tile but appears in Settings → Apps; disabling a Home app removes its tile on the next Home visit; enabling restores it; the setting survives reboot and a card swap.
- A non-boolean `show_in_home` makes the package disappear from Home and Settings.
- From the launcher: `status` reports each field correctly for the helper, an unknown ID, an invalid package, and a disabled app; `open` starts the helper, refuses a disabled app and its own ID, and closing the helper returns to Home.
- `open` while a recording or network request is running ends them cleanly before the target starts.
