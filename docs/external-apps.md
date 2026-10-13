# SD card apps (SDK 1 through 4)

Copy a folder such as `examples/apps/counter` to `/Apps/counter` on a FAT32 SD card. **Counter** appears directly in Home's combined built-in/Lua app grid. Home scans up to 128 entries in `/Apps`, shows up to 32 runnable packages, sorts all tiles case-insensitively by display name, and keeps duplicate names. Drag horizontally between nine-tile pages; the grid follows the finger, advances after 48 px or a short fast flick, and rubber-bands without wrapping at either end. The catalog refreshes when Home opens or the card is remounted. Identical rescans leave the retained Home cache untouched; a changed catalog redraws only grid positions whose sorted tile content changed. A package can be updated by replacing its files on SD; no firmware flash is needed. USB Drive transfer still uses its normal isolated boot and restart.

## Package contract

An app is `/Apps/<id>/manifest.json` plus `/Apps/<id>/main.lua` and normally `/Apps/<id>/logo.png`. The manifest is UTF-8 JSON with `id`, `name`, integer `sdk`, and integer RGB565 `app_icon_bg` fields, plus an optional `permissions` string array that only SDK 4 reads. `app_icon_bg` ranges from 0 through 65535. The logo is an exact 64×64, non-interlaced PNG with alpha; Home composites it over the 80px circle using `app_icon_bg`. A missing or invalid background uses the standard purple Apps color, while a missing, corrupt, wrong-sized, interlaced, non-alpha, or undecodable logo uses the white generic Apps glyph. These visual fallbacks do not prevent an otherwise valid package from running.

SDK 1 through 4 are accepted; the original counter example remains SDK 1, `examples/apps/workout` uses SDK 2, `examples/apps/recorder` uses SDK 3, and `examples/apps/files` uses SDK 4. The ID must match its lowercase folder name and match `[a-z][a-z0-9_-]{0,31}`. Names are 1–40 UTF-8 bytes. The manifest is at most 2 KiB and `main.lua` at most 64 KiB. Only Lua source text is accepted; bytecode and additional Lua modules are unavailable. Home omits packages that fail these core checks or have a missing/oversized `main.lua`.

`main.lua` returns a table with a required `on_start()` function and optional `on_action(id)` and `on_tick(now_ms)` functions. SDK 2 and later additionally permit `on_touch(x, y)` for touch-down edges inside a presented canvas, with coordinates relative to the area below the app bar. SDK 4 adds `on_touch_end(x, y, held_ms)`, `on_input(text)`, and `on_resume()`. `on_tick` runs about every 100 ms while the app is open. Each callback has a 100,000-instruction and 20 ms script-execution limit; bounded host file I/O and saved-volume writes do not count toward the time limit. The Lua state has a 512 KiB PSRAM allocation limit. A limit or script error stops callbacks and displays an error; closing releases its task and memory.

SDK 1 provides:

| Function | Contract |
| --- | --- |
| `esp.ui.show(rows)` | Replace the whole page. Supply up to ten array rows, each `{kind="text", text="..."}` or `{kind="button", id="...", text="..."}`. Text is 1–96 bytes; button IDs are 1–32 bytes. Optional `rows.title` is at most 40 bytes. The host draws the app bar, 40px rows, and button cells with `UiKit`; touches on buttons call `on_action(id)`. |
| `esp.storage.read(name)` | Return file bytes, or `nil, error`. Reads at most 16 KiB. |
| `esp.storage.write(name, bytes)` | Return `true`, or `false, error`. Writes at most 16 KiB through a `.tmp` file and rename. |
| `esp.device.status()` | Return smooth `battery_voltage_mv`, `wifi_connected`, `sd_mounted`, and `epoch_seconds` (zero until time is valid). Voltage alone does not establish state of charge or charging state. |
| `esp.time.millis()` | Return the device's monotonic millisecond counter. |

Storage names are 1–64 ASCII bytes using letters, digits, `_`, `-`, or `.`; `.` and `..` are rejected. They address only `/.system/appdata/<id>/<name>`. An SDK 1–3 app cannot access package source files, other apps' data, or the general SD filesystem; SDK 4 opens ordinary SD storage only to a package that declares the `files` permission (see below). No SDK reaches `SecretStorage`, `/.system` outside the app's own data folder, or other hidden folders. The host loads selected safe Lua libraries (`table`, `string`, `math`, and `utf8`); Lua file, OS, package, debug, dynamic load, and native-module interfaces are unavailable. These limits are for personal or trusted apps in the firmware process, not hostile-code isolation.

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

## SDK 3 microphone and WAV files

SDK 3 keeps every SDK 2 operation and adds a microphone, WAV playback, and a file-size query. Raw audio never crosses into Lua: the app's worker task drains the microphone, writes the file, and feeds the speaker, while Lua starts, stops, and polls. Recordings are 16 kHz mono 16-bit PCM WAV files in the app's own data folder, so names follow the storage rules above and the 16 KiB `esp.storage.read` limit does not apply to them (read them only through `esp.audio.play_file`).

| Operation | Contract |
| --- | --- |
| `esp.storage.size(name)` | Return the byte size of an app data file, or `nil, error` (`File not found`, `SD busy`, …). |
| `esp.mic.status()` | Return `ready`, `powered` (the physical mic switch; always true when the board has no sense line), `recording`, `elapsed_ms`, `bytes`, `level`, and, when the previous recording ended abnormally or empty, `error`. `level` is 0–100 and is zero when not recording. |
| `esp.mic.start([name [, max_seconds]])` | Start capturing for 1–600 seconds (default 600). With `name`, write `<name>` through `<name>.tmp` and rename on stop, so a failed recording keeps the previous file. Without `name`, only measure `level`/`elapsed_ms` and write nothing. Return `true` or `false, error` (`Turn mic power switch on`, `Microphone busy`, `SD card unavailable`, …). |
| `esp.mic.stop()` | Finalize and rename the file, returning `true, bytes`, or `false, error` (`Not recording`, `Recording was empty`, `Recording could not be saved`). |
| `esp.audio.play_file(name)` | Play a mono 16-bit PCM WAV of 8–44.1 kHz written with a 44-byte header (the format `esp.mic.start` produces). A header that claims more data than the file holds is clamped to the file size. Return `true` or `false, error`. |
| `esp.audio.stop_file()` | Stop file playback. |
| `esp.audio.playback()` | Return `playing`, `finished` (true after natural completion until the next start), `elapsed_ms`, and, after a failure, `error`. |

`level` is the peak of the most recent samples on a 60 dB scale (0 at −60 dBFS, 100 at full scale) with a fall time of about 0.8 s. The microphone and speaker share clock pins and never run together: `esp.mic.start` stops tones and file playback, while `esp.audio.play_tone`, `esp.audio.play_file`, and tone service refuse to start (`audio busy` / `Microphone is recording`) during a recording. The host also refuses to start if the Assistant or another component is already capturing. A recording ends by itself, keeping the captured audio, when the mic switch turns off (`Mic switch turned off`), the time limit is reached (`Recording limit reached`), or a write fails; an SD removal closes the app. A callback error, closing the app, or leaving it finalizes the file and releases the microphone. SD work inside `mic.start`/`mic.stop`/`play_file` does not count toward the callback time limit; capture and playback chunks lock `SpiBusLock` for at most 1 KiB at a time and keep up to 1 KiB buffered when the bus is busy. A long SD stall can still overrun the microphone's roughly 64 ms DMA buffer and drop samples.

`examples/apps/recorder` rebuilds the built-in Voice Recorder (Rec/Stop/Play, mic-switch handling, storage and detail lines) with `esp.mic`, `esp.audio.play_file`, and a level meter, saving to `/.system/appdata/recorder/voice_last.wav`. It does not use the name-less monitor mode.

## SDK 4 general file access

SDK 4 keeps every SDK 3 operation. It adds general SD access under `esp.fs`, host-owned navigation to the built-in viewers and keyboard, a `UiKit` list cell for the canvas, and touch-release reporting. `esp.fs` exists only when the manifest declares both `"sdk": 4` and `"permissions": ["files"]`; unknown permission strings are ignored. The other SDK 4 additions need no permission.

### Paths

Every `esp.fs` path is an absolute SD path of 1–255 bytes, such as `/Music/song.mp3`. The host accepts a path only when all of these hold:

- No segment starts with `.` or ends with `.tmp`. This excludes `/.configs` (secret storage), `/.system`, `.cache` sidecar folders, and macOS metadata.
- No segment is empty, starts with a space, or ends with a space or dot, and no byte is a control character or one of `\ : * ? " < > |`. FAT would otherwise map such text onto a different entry than the one checked.
- `/` itself is accepted only by `scan` and `stat`.

`mkdir`, `rename`, and `remove` additionally refuse `/` and anything at or below `/Apps` (compared case-insensitively) with `Path is read-only`, so an app can list and open package folders but never change its own or another package. A rename target may not use an internal cache extension (`.em16`, `.emga`, `.empa`, `.eth1`). Listings omit hidden names, internal cache files, and entries whose full path would exceed 255 bytes.

### Operations

| Operation | Contract |
| --- | --- |
| `esp.fs.scan(path)` | Start listing a folder in the app worker, replacing any previous listing. Return `true` or `false, error` (`Invalid path`, `Folder not found`, `File job active`, `SD busy`, …). |
| `esp.fs.scan_status()` | Return `scanning`, `ready`, `count`, `truncated`, `path`, and `error` after a failed scan. A finished listing holds at most 2,000 entries; `truncated` reports that the folder had more. |
| `esp.fs.entries(offset, count)` | Return an array of up to 16 entries from the finished listing, skipping `offset` (zero-based) entries, or `nil, error` (`No listing`, `Invalid range`). Folders come first; names use the Files screen's case-folded natural order. |
| `esp.fs.stat(path)` | Return one entry table, or `nil, error` (`File not found`, …). |
| `esp.fs.mkdir(path)` | Create one folder whose parent exists. Return `true` or `false, error` (`Already exists`, `Cannot create folder`, …). |
| `esp.fs.rename(from, to)` | Start a rename or move job. Return `true` or `false, error` (`File not found`, `Target exists`, `Cannot move a folder into itself`, `File job active`, …). A change of letter case only is reported as `Target exists`. |
| `esp.fs.remove(path)` | Start a delete job. A folder is removed with everything inside it, including hidden entries, up to 16 nested levels (`Folder too deep`). |
| `esp.fs.job()` | Return `active`, `kind` (`"rename"` or `"remove"` for the latest job), `done` (items removed or renamed so far), `finished` (the latest job completed), and `error` when it failed (`Rename failed`, `Delete failed`, `Cache cleanup failed`, `Cancelled`, `SD card removed`). |
| `esp.fs.cancel()` | Stop a running scan and ask the running job to stop before its next step. A cancelled folder delete keeps what it had not yet removed. |
| `esp.fs.open(path)` | Ask the host to open an image, audio, video, or text file in its built-in viewer. Return `true` once the request is accepted, or `false, error` (`Unsupported file`, `File not found`, `File job active`, `Navigation pending`). |

An entry table has `name`, `dir`, `size` (bytes, 0 for folders), `modified` (the FAT timestamp as epoch seconds, 0 when unknown), `kind` (`"folder"`, `"image"`, `"audio"`, `"video"`, `"text"`, or `"other"`, from `FileFormats`), and `openable`.

One scan and one job exist at a time. `mkdir` and every started job discard the current listing, so scan again afterwards. Scans and jobs advance in the worker between callbacks, one SD transaction per `SpiBusLock` hold and about 8 ms per pass, so `on_tick` keeps running; poll `scan_status` and `job`. Before a file is renamed or removed the host removes its `.cache` sidecars through `MediaCache`, and before a rename it also clears stale sidecars under the target name. Renaming or moving a folder carries its nested `.cache` folders with it.

### Host navigation, keyboard, and resume

| Operation | Contract |
| --- | --- |
| `esp.ui.prompt(title [, initial])` | Ask the host to open the keyboard. `title` is 1–40 bytes and `initial` at most 255. Return `true`, or `false, error` when another request is pending. |
| `on_input(text)` | Called after the keyboard closes: the entered text (cut to 255 bytes), or `nil` when it was cancelled. |
| `on_resume()` | Called after `on_input`, and after a viewer opened by `esp.fs.open` closes. |

A request is carried out after the current callback returns. The worker first finalizes any recording and stops tones and file playback, then pauses: no callbacks run, and scans stay suspended, while the keyboard or viewer covers the app. The app's Lua state and last frame stay in memory during that time. When the covering screen closes, the host redraws the last frame and resumes the app. If the screen cannot be opened, the app resumes at once (`on_input(nil)` for a prompt, then `on_resume()`). SD removal while covered ends the app as usual. An image opened this way has no previous/next neighbours.

### Canvas additions

| Operation | Contract |
| --- | --- |
| `esp.ui.list_cell(y, label [, icon [, action]])` | Draw a full-width 40px `UiKit` list cell at canvas `y` on a black background. `icon` is `"none"` (default), `"folder"`, `"file"`, `"image"`, `"audio"`, `"video"`, or `"text"` and selects the Files folder/document icon and tint. `action` is `"none"` (default), `"more"`, or `"close"` and draws a kit glyph in the trailing 40×40 cell. The label is 1–96 bytes and is ellipsized by the host. |
| `on_touch_end(x, y, held_ms)` | Called when a touch that began on the canvas is released, with its last canvas position and duration. Together with `on_touch` and `on_tick` this supports hold gestures. |

The app owns hit testing: the whole 40px row, and the whole trailing 40×40 cell when it draws an action.

`examples/apps/files` rebuilds the Files screen for ordinary storage with these operations: an eight-row paged listing, opening folders and media, New folder and Rename through the keyboard, Move by choosing a destination folder, and hold-to-delete. It has no thumbnails and no secret storage.

## Future SDK versions

Extend the versioned `esp` API through controller-owned services. LED, Wi-Fi/network, arbitrary file content read/write, file copy, and thumbnails remain future candidates. Built-in browser and assistant operations and the exclusive USB Drive transition need explicit host workflows. Never expose raw GPIO/SPI controllers or protected files as pointers or unrestricted paths; new file operations must validate through `PathPolicy`. Add an example, device verification, and an updated contract here for each version.
