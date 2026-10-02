# SD card apps (SDK 1)

Copy a folder such as `examples/apps/counter` to `/Apps/counter` on a FAT32 SD card. Open **Apps** on Home, then tap **Counter**. The launcher scans up to 128 entries in `/Apps`, shows up to 32 app folders, and refreshes when opened or when the card is remounted. A package can be updated by replacing its files on SD; no firmware flash is needed. USB Drive transfer still uses its normal isolated boot and restart.

## Package contract

An app is `/Apps/<id>/manifest.json` plus `/Apps/<id>/main.lua`. The manifest is UTF-8 JSON with `id`, `name`, and integer `sdk: 1` fields, for example `{"id":"counter","name":"Counter","sdk":1}`. The ID must match its lowercase folder name and match `[a-z][a-z0-9_-]{0,31}`. Names are 1–40 UTF-8 bytes. The manifest is at most 2 KiB and `main.lua` at most 64 KiB. Only Lua source text is accepted; bytecode and additional Lua modules are unavailable.

`main.lua` returns a table with a required `on_start()` function and optional `on_action(id)` and `on_tick(now_ms)` functions. `on_tick` runs about every 100 ms while the app is open. Each callback has a 100,000-instruction and 20 ms script-execution limit; bounded host file I/O does not count toward the time limit. The Lua state has a 512 KiB PSRAM allocation limit. A limit or script error stops callbacks and displays an error; closing releases its task and memory.

SDK 1 provides:

| Function | Contract |
| --- | --- |
| `esp.ui.show(rows)` | Replace the whole page. Supply up to ten array rows, each `{kind="text", text="..."}` or `{kind="button", id="...", text="..."}`. Text is 1–96 bytes; button IDs are 1–32 bytes. Optional `rows.title` is at most 40 bytes. The host draws the app bar, 40px rows, and button cells with `UiKit`; touches on buttons call `on_action(id)`. |
| `esp.storage.read(name)` | Return file bytes, or `nil, error`. Reads at most 16 KiB. |
| `esp.storage.write(name, bytes)` | Return `true`, or `false, error`. Writes at most 16 KiB through a `.tmp` file and rename. |
| `esp.device.status()` | Return smooth `battery_voltage_mv`, `wifi_connected`, `sd_mounted`, and `epoch_seconds` (zero until time is valid). Voltage alone does not establish state of charge or charging state. |
| `esp.time.millis()` | Return the device's monotonic millisecond counter. |

Storage names are 1–64 ASCII bytes using letters, digits, `_`, `-`, or `.`; `.` and `..` are rejected. They address only `/.system/appdata/<id>/<name>`. An app cannot access package source files, other apps' data, the general SD filesystem, or `SecretStorage` through SDK 1. The host loads selected safe Lua libraries (`table`, `string`, `math`, and `utf8`); Lua file, OS, package, debug, dynamic load, and native-module interfaces are unavailable. These limits are for personal or trusted apps in the firmware process, not hostile-code isolation.

Apps run only while open. Closing an app or removing its SD card stops callbacks and releases the Lua state. Reinsert the card and reopen the app to start again. App work uses a separate task; the UI task owns touch and drawing. Each SD transaction takes the shared `SpiBusLock` and releases it between bounded chunks.

## Future SDK versions

Extend the versioned `esp` API through controller-owned services: LED and Wi-Fi/network; then microphone, speaker, recording, and media. Built-in browser and assistant operations and the exclusive USB Drive transition need explicit host workflows. Never expose raw GPIO/SPI controllers or protected files as pointers or unrestricted paths. Add an example, device verification, and an updated contract here for each version.
