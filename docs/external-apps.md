# SD card apps (SDK 1 and 2)

Copy a folder such as `examples/apps/counter` to `/Apps/counter` on a FAT32 SD card. **Counter** appears directly in Home's combined built-in/Lua app grid. Home scans up to 128 entries in `/Apps`, shows up to 32 runnable packages, sorts all tiles case-insensitively by display name, and keeps duplicate names. Swipe horizontally between nine-tile pages. The catalog refreshes when Home opens or the card is remounted. A package can be updated by replacing its files on SD; no firmware flash is needed. USB Drive transfer still uses its normal isolated boot and restart.

## Package contract

An app is `/Apps/<id>/manifest.json` plus `/Apps/<id>/main.lua` and normally `/Apps/<id>/logo.png`. The manifest is UTF-8 JSON with `id`, `name`, integer `sdk`, and integer RGB565 `app_icon_bg` fields. `app_icon_bg` ranges from 0 through 65535. The logo is an exact 64×64, non-interlaced PNG with alpha; Home composites it over the 80px circle using `app_icon_bg`. A missing or invalid background uses the standard purple Apps color, while a missing, corrupt, wrong-sized, interlaced, non-alpha, or undecodable logo uses the white generic Apps glyph. These visual fallbacks do not prevent an otherwise valid package from running.

SDK 1 and 2 are accepted; the original counter example remains SDK 1 and `examples/apps/workout` uses SDK 2. The ID must match its lowercase folder name and match `[a-z][a-z0-9_-]{0,31}`. Names are 1–40 UTF-8 bytes. The manifest is at most 2 KiB and `main.lua` at most 64 KiB. Only Lua source text is accepted; bytecode and additional Lua modules are unavailable. Home omits packages that fail these core checks or have a missing/oversized `main.lua`.

`main.lua` returns a table with a required `on_start()` function and optional `on_action(id)` and `on_tick(now_ms)` functions. SDK 2 additionally permits `on_touch(x, y)` for touch-down edges inside a presented canvas, with coordinates relative to the area below the app bar. `on_tick` runs about every 100 ms while the app is open. Each callback has a 100,000-instruction and 20 ms script-execution limit; bounded host file I/O and saved-volume writes do not count toward the time limit. The Lua state has a 512 KiB PSRAM allocation limit. A limit or script error stops callbacks and displays an error; closing releases its task and memory.

SDK 1 provides:

| Function | Contract |
| --- | --- |
| `esp.ui.show(rows)` | Replace the whole page. Supply up to ten array rows, each `{kind="text", text="..."}` or `{kind="button", id="...", text="..."}`. Text is 1–96 bytes; button IDs are 1–32 bytes. Optional `rows.title` is at most 40 bytes. The host draws the app bar, 40px rows, and button cells with `UiKit`; touches on buttons call `on_action(id)`. |
| `esp.storage.read(name)` | Return file bytes, or `nil, error`. Reads at most 16 KiB. |
| `esp.storage.write(name, bytes)` | Return `true`, or `false, error`. Writes at most 16 KiB through a `.tmp` file and rename. |
| `esp.device.status()` | Return smooth `battery_voltage_mv`, `wifi_connected`, `sd_mounted`, and `epoch_seconds` (zero until time is valid). Voltage alone does not establish state of charge or charging state. |
| `esp.time.millis()` | Return the device's monotonic millisecond counter. |

Storage names are 1–64 ASCII bytes using letters, digits, `_`, `-`, or `.`; `.` and `..` are rejected. They address only `/.system/appdata/<id>/<name>`. An app cannot access package source files, other apps' data, the general SD filesystem, or `SecretStorage` through either SDK. The host loads selected safe Lua libraries (`table`, `string`, `math`, and `utf8`); Lua file, OS, package, debug, dynamic load, and native-module interfaces are unavailable. These limits are for personal or trusted apps in the firmware process, not hostile-code isolation.

Apps run only while open. Closing an app or removing its SD card stops callbacks and releases the Lua state. Reinsert the card and reopen the app to start again. App work uses a separate task; the UI task owns touch and drawing. Each SD transaction takes the shared `SpiBusLock` and releases it between bounded chunks.

## SDK 2 canvas and audio

SDK 2 keeps every SDK 1 operation. Its canvas is 320×432 in portrait, with `(0, 0)` directly below the host-owned 48px app bar. `esp.tft.begin_frame(background_rgb565, title)` starts a complete replacement frame; `esp.tft.present()` publishes it. A frame has at most 64 drawing commands and a 400,000-pixel estimated drawing budget. The UI task draws only the latest complete frame and clips every command to the canvas. RGB565 colors are integers from 0 to 65535. A drawing error stops the app, as with other invalid SDK calls. For smooth primitives, supply the color immediately behind the shape as the final background argument.

| SDK 2 operation | Arguments after the function name |
| --- | --- |
| `esp.tft.fillScreen` | `color` (canvas only) |
| `fillRect`, `drawRect`, `drawLine` | `x, y, width_or_x2, height_or_y2, color` |
| `drawFastHLine`, `drawFastVLine` | `x, y, length, color` |
| `drawCircle`, `fillCircle` | `x, y, radius, color` |
| `drawRoundRect`, `fillRoundRect` | `x, y, width, height, radius, color` |
| `fillTriangle` | `x1, y1, x2, y2, x3, y3, color` |
| `drawWideLine` | `x1, y1, x2, y2, width, color, background` |
| `drawSmoothCircle`, `fillSmoothCircle` | `x, y, radius, color, background` |
| `drawSmoothRoundRect` | `x, y, outer_radius, inner_radius, width, height, color, background` |
| `fillSmoothRoundRect` | `x, y, width, height, radius, color, background` |
| `drawSmoothArc` | `x, y, outer_radius, inner_radius, start_degrees, end_degrees, color, background, round_ends` (0 or 1) |
| `drawString`, `drawNumber` | `text_or_number, x, y, color, background, size, datum` |
| `esp.ui.button` | `x, y, width, height, label, fill, text_color, background` |
| `esp.ui.progress` | `x, y, width, height, percent, fill, frame, background` |

Canvas text and button labels are limited to 96 bytes. Text uses `UiText` sizes `0=Compact`, `1=Content`, `2=Title` and TFT_eSPI datum values `0–8`; `4` centers text at `(x, y)`. Buttons and progress bars use `UiKit`. Coordinates are bounded to −512…1024, dimensions to 1…640, and radii to 1…480 before drawing; the viewport applies the final screen clip. Drawing calls only record commands; they do not touch TFT_eSPI from Lua's worker. The app handles its own content hit regions in `on_touch`, while the host owns the close target. Bitmap buffers, rotation, font loading, TFT transactions, raw pointers, and display readback are unavailable.

| Audio operation | Contract |
| --- | --- |
| `esp.audio.play_tone(frequency_hz, duration_ms)` | Queue a 100–4000 Hz tone lasting 10–2000 ms. Return `true` or `false, error`; at most eight cues may wait. Playback uses non-blocking 16 kHz mono I2S writes. |
| `esp.audio.stop_tones()` | Cancel the current and queued cues and stop speaker output. |
| `esp.audio.volume()` | Return the saved device speaker volume, 0–10. |
| `esp.audio.set_volume(level)` | Persist a 0–10 volume and apply it to the shared speaker. Return `true` or `false, error`. |

Closing an app, a callback error, or SD removal stops its tones. The built-in Workout screen and other audio users retain their normal screen ownership; an app uses the speaker only while its host screen is active.

## Future SDK versions

Extend the versioned `esp` API through controller-owned services. LED and Wi-Fi/network, microphone, recording, and media remain future candidates. Built-in browser and assistant operations and the exclusive USB Drive transition need explicit host workflows. Never expose raw GPIO/SPI controllers or protected files as pointers or unrestricted paths. Add an example, device verification, and an updated contract here for each version.
