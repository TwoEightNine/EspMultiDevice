# SD card apps (SDK 1 through 6)

Copy a folder such as `examples/apps/counter` to `/Apps/counter` on a FAT32 SD card. **Counter** appears directly in Home's combined built-in/Lua app grid. Home scans up to 128 entries in `/Apps`, shows up to 32 runnable packages, sorts all tiles case-insensitively by display name, and keeps duplicate names. Drag horizontally between nine-tile pages; the grid follows the finger, advances after 48 px or a short fast flick, and rubber-bands without wrapping at either end. The catalog refreshes when Home opens or the card is remounted. Identical rescans leave the retained Home cache untouched; a changed catalog redraws only grid positions whose sorted tile content changed. A package can be updated by replacing its files on SD; no firmware flash is needed. USB Drive transfer still uses its normal isolated boot and restart.

## Package contract

An app is `/Apps/<id>/manifest.json` plus `/Apps/<id>/main.lua` and normally `/Apps/<id>/logo.png`. The manifest is UTF-8 JSON with `id`, `name`, integer `sdk`, and integer RGB565 `app_icon_bg` fields, plus an optional `permissions` string array that SDK 4 and later read and, for the SDK 6 `network` permission, a `hosts` array. `app_icon_bg` ranges from 0 through 65535. The logo is an exact 64×64, non-interlaced PNG with alpha; Home composites it over the 80px circle using `app_icon_bg`. A missing or invalid background uses the standard purple Apps color, while a missing, corrupt, wrong-sized, interlaced, non-alpha, or undecodable logo uses the white generic Apps glyph. These visual fallbacks do not prevent an otherwise valid package from running.

SDK 1 through 6 are accepted; the original counter example remains SDK 1, `examples/apps/workout` uses SDK 2, `examples/apps/recorder` uses SDK 3, `examples/apps/files` uses SDK 4, `examples/apps/images` uses SDK 5, and `examples/apps/dvach` uses SDK 6. The ID must match its lowercase folder name and match `[a-z][a-z0-9_-]{0,31}`. Names are 1–40 UTF-8 bytes. The manifest is at most 2 KiB and `main.lua` at most 64 KiB. Only Lua source text is accepted; bytecode and additional Lua modules are unavailable. Home omits packages that fail these core checks or have a missing/oversized `main.lua`.

`main.lua` returns a table with a required `on_start()` function and optional `on_action(id)` and `on_tick(now_ms)` functions. SDK 2 and later additionally permit `on_touch(x, y)` for touch-down edges inside a presented canvas, with coordinates relative to the area below the app bar. SDK 4 adds `on_touch_end(x, y, held_ms)`, `on_input(text)`, and `on_resume()`. `on_tick` runs about every 100 ms while the app is open. Each callback has a 100,000-instruction and 20 ms script-execution limit. The 20 ms is the app worker's own CPU time (from the FreeRTOS run-time counter), so time in which Wi-Fi, TLS, or other tasks preempt the script is not charged; bounded host file I/O and saved-volume writes do not count toward it either. Before each callback the host runs the Lua collector until its outstanding debt is paid, so garbage-collection work caused by host allocations (compiling the app, building SDK result tables) is not charged to the callback. The Lua state has a 512 KiB PSRAM allocation limit. A limit or script error stops callbacks and displays an error page with *Restart app* (a fresh Lua state) and *Close app*; closing releases its task and memory.

SDK 1 provides:

| Function | Contract |
| --- | --- |
| `esp.ui.show(rows)` | Replace the whole page. Supply up to ten array rows, each `{kind="text", text="..."}` or `{kind="button", id="...", text="..."}`. Text is 1–96 bytes; button IDs are 1–32 bytes. Optional `rows.title` is at most 40 bytes. The host draws the app bar, 40px rows, and button cells with `UiKit`; touches on buttons call `on_action(id)`. |
| `esp.storage.read(name)` | Return file bytes, or `nil, error`. Reads at most 16 KiB. |
| `esp.storage.write(name, bytes)` | Return `true`, or `false, error`. Writes at most 16 KiB through a `.tmp` file and rename. |
| `esp.device.status()` | Return smooth `battery_voltage_mv`, `wifi_connected`, `sd_mounted`, and `epoch_seconds` (zero until time is valid). Voltage alone does not establish state of charge or charging state. |
| `esp.time.millis()` | Return the device's monotonic millisecond counter. |

Storage names are 1–64 ASCII bytes using letters, digits, `_`, `-`, or `.`; `.` and `..` are rejected. They address only `/.system/appdata/<id>/<name>`. An SDK 1–3 app cannot access package source files, other apps' data, or the general SD filesystem; SDK 4 and later open ordinary SD storage only to a package that declares the `files` permission (see below), and SDK 6 reaches the network only for a package that declares `network` and lists its hosts. No SDK reaches `SecretStorage`, `/.system` outside the app's own data folder, or other hidden folders. The host loads selected safe Lua libraries (`table`, `string`, `math`, and `utf8`); Lua file, OS, package, debug, dynamic load, and native-module interfaces are unavailable. These limits are for personal or trusted apps in the firmware process, not hostile-code isolation.

Apps run only while open. Closing an app or removing its SD card stops callbacks and releases the Lua state. Reinsert the card and reopen the app to start again. App work uses a separate task; the UI task owns touch and drawing. Each SD transaction takes the shared `SpiBusLock` and releases it between bounded chunks. Loading a package and the one-shot storage and file calls wait up to 250 ms for the bus (battery-history appends hold it for about 50 ms every second) before reporting `SD busy`; that wait does not count toward the callback time limit.

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
| `on_input(text)` | Called after the keyboard closes: the entered text (cut to 255 bytes on a character boundary; 1,024 bytes for SDK 6), or `nil` when it was cancelled. |
| `on_resume()` | Called after `on_input`, and after a viewer opened by `esp.fs.open` closes. |

A request is carried out after the current callback returns. The worker first finalizes any recording and stops tones and file playback, then pauses: no callbacks run, and scans stay suspended, while the keyboard or viewer covers the app. The app's Lua state and last frame stay in memory during that time. When the covering screen closes, the host redraws the last frame and resumes the app. If the screen cannot be opened, the app resumes at once (`on_input(nil)` for a prompt, then `on_resume()`). SD removal while covered ends the app as usual. An image opened this way has no previous/next neighbours.

### Canvas additions

| Operation | Contract |
| --- | --- |
| `esp.ui.list_cell(y, label [, icon [, action]])` | Draw a full-width 40px `UiKit` list cell at canvas `y` on a black background. `icon` is `"none"` (default), `"folder"`, `"file"`, `"image"`, `"audio"`, `"video"`, or `"text"` and selects the Files folder/document icon and tint. `action` is `"none"` (default), `"more"`, or `"close"` and draws a kit glyph in the trailing 40×40 cell. The label is 1–96 bytes and is ellipsized by the host. |
| `on_touch_end(x, y, held_ms)` | Called when a touch that began on the canvas is released, with its last canvas position and duration. Together with `on_touch` and `on_tick` this supports hold gestures. |

The app owns hit testing: the whole 40px row, and the whole trailing 40×40 cell when it draws an action.

`examples/apps/files` rebuilds the Files screen for ordinary storage with these operations: an eight-row paged listing, opening folders and media, New folder and Rename through the keyboard, Move by choosing a destination folder, and hold-to-delete. It has no thumbnails and no secret storage.

## SDK 5 host image layer

SDK 5 keeps every SDK 4 operation and adds `esp.image`, which shows one image file on the whole display through the same pipeline as the built-in Image Viewer. `esp.image` exists only when the manifest declares both `"sdk": 5` and `"permissions": ["files"]`. Pixels never cross into Lua: the host decodes, scales, caches, and animates, while Lua chooses the file, the bar state, and the zoom, and decides what the bar's targets and the image touches do.

| Operation | Contract |
| --- | --- |
| `esp.image.open(path)` | Show the image layer and start loading `path`, aspect-fit and unzoomed, replacing any image already shown. The path follows the `esp.fs` rules and must be an openable image per `FileFormats`. Return `true` once the request is accepted, or `false, error` (`Invalid path`, `Unsupported file`, `File not found`, `File job active`, `SD busy`). A file that cannot be decoded is reported through `status`, not here. |
| `esp.image.status()` | Return `state` (`"none"`, `"loading"`, `"ready"`, or `"error"`), `path`, source `width` and `height`, `format` (`"BMP"`, `"PNG"`, `"JPEG"`, `"GIF"`, `"WebP"`, or `"unknown"`), `animated`, `zoom`, `zoom_available`, `bar`, and `error` in the error state. |
| `esp.image.set_zoom(on)` | Switch centered crop-to-fill zoom on or off. Return `true`, or `false, error` (`No image`, `Zoom unavailable`). |
| `esp.image.set_bar(visible)` | Show the app bar over the image (informative mode) or hide it (full screen) without reloading. The setting carries over to the next `open`. |
| `esp.image.close()` | Remove the layer, release its buffers, and redraw the app's latest frame or page. The bar setting returns to visible. |

Formats, limits, and caches are those of the built-in viewer (see [media.md](media.md)): still images fill the 320×480 display aspect-fit, animations play in the centered 320×240 photo viewport, and the layer reads and writes the same validated `.cache` sidecars, so an image first opened by either viewer is a cache hit in the other. Animated GIF and WebP stream from their source with no sidecar, and `state` stays `"loading"` until the first frame is shown. `zoom_available` is true for a loaded still image whose aspect ratio differs from the display's by more than 10%. The host draws the loading progress and, for a file it cannot show, the viewer's error panel.

While the layer is shown:

- Frames and pages the app publishes are kept but not drawn until `close`.
- A visible bar shows the file name, the Close icon, and the zoom glyph when zoom is available. Their 48px targets call `on_action("image.close")` and `on_action("image.zoom")`; the host neither closes the app nor zooms by itself. An app that shows an image must therefore handle `image.close`. The rest of the bar is inert.
- Every other touch-down calls `on_touch(x, y)` in the usual canvas coordinates. With the bar hidden the top 48px of the display are part of the image and report `y` from −48 to −1. `on_touch_end` follows the same rule.

Requests are carried out by the UI task after the callback returns, so `status` reports `"loading"` until the host has processed the latest `open`. PNG, JPEG, and still WebP decode in one step: callbacks keep running, but the host presents nothing and takes no touch until the image appears. The host closes the layer when the app stops and when `esp.fs.open` or `esp.ui.prompt` covers the app; after such a cover `status().state` is `"none"` in `on_resume()`. Scans, jobs, and audio may run while an image loads; each side holds `SpiBusLock` for one transaction at a time. Images in secret storage are not reachable.

`examples/apps/images` rebuilds the Image Viewer with these operations. Because Home starts an app without a file, it begins with a folder picker that lists folders and openable images, eight rows per page, and remembers the last folder in its app data. In the viewer a tap on either 48px edge moves to the previous or next image of the folder with wrap, keeping the bar state and resetting zoom; a tap in between toggles informative and full-screen mode; the bar's zoom target toggles crop zoom; Close returns to the picker page holding that image.

## SDK 6 network, parsing, text, and bitmaps

SDK 6 keeps every SDK 5 operation. It adds HTTP(S) requests, JSON/URL/Base64 codecs, hashes, HTML-to-text, wrapped text, and decoded bitmaps. Only `esp.net` needs a permission: it exists when the manifest declares `"sdk": 6`, `"permissions": ["network"]`, and a `"hosts"` array of 1–8 exact lowercase host names or IPv4 literals, for example `["2ch.su", "2ch.hk", "2ch.org"]`. A `network` permission without a valid `hosts` array makes the package invalid. Every request URL and every redirect target must use a listed host (any port).

### Requests

| Operation | Contract |
| --- | --- |
| `esp.net.request(options)` | Queue a request and return its id (a positive integer), or `nil, error` (`Host not allowed`, `Invalid URL`, `Invalid header`, `Too many headers`, `Body too large`, `Too many requests`, `Wi-Fi not connected`, `App memory unavailable`). `options`: `url`; `method` (`"GET"`, `"POST"`, `"PUT"`, `"PATCH"`, `"DELETE"`; default GET); `headers` (string-to-string table); `body` (string) or `multipart` (an array of 1–32 `{name, value [, filename] [, content_type]}` text parts, sent as `multipart/form-data` with a host boundary); `timeout_ms` (1,000–60,000, default 20,000); `max_bytes` (text body cap, 1–98,304, default 32,768); `redirects` (default `true` for GET, `false` otherwise); and `json = {keep = {key, …}, max_string = n, max_items = n}`. |
| `esp.net.get(url [, options])` | `request` with `method = "GET"`. |
| `esp.net.post(url, body [, options])` / `esp.net.put(url, body [, options])` | `request` with that method and body. A body without an explicit `Content-Type` header is sent as `application/x-www-form-urlencoded`. |
| `esp.net.status(id)` | `{state, received, total}` while the request is queued or running (`state` is `"queued"`, `"connecting"`, `"sending"`, or `"receiving"`; `total` is −1 when unknown), otherwise `nil`. |
| `esp.net.cancel([id])` | Cancel one request, or every request. A cancelled request still reports `on_response` with `error = "Cancelled"`. |
| `on_response(id, response)` | Called once per request. `response` has `ok` (no transport error and a 2xx status), `status` (0 when no response arrived), `url` (after redirects), `headers` (lowercase names; repeated headers such as `set-cookie` are joined by newlines; at most 32 and 4 KiB), `truncated`, and, depending on the mode, `body` or `json`. `error` is present on failure: `Timeout`, `Cannot connect`, `DNS lookup failed`, `TLS failed`, `Clock not set`, `Wi-Fi not connected`, `Redirect not allowed`, `Too many redirects`, `Compressed response`, `Response too large`, `Invalid JSON`, `JSON too deep`, `JSON too large`, `Invalid UTF-8`, `App memory full`, or `Cancelled`. |

Requests run one at a time in submission order on a per-app `app-net` task (16 KiB stack, core 0), created on the first request and deleted when the app ends, so a connect or a slow server never blocks callbacks or the UI. At most four requests may be queued or running. Results wait while the app is paused under the keyboard or a viewer and arrive after `on_resume()`. Building the response table is host work outside the 20 ms limit; if it would exhaust the Lua heap, `on_response` receives `{ok = false, status = 0, error = "App memory full"}` instead.

The host sets `Host`, `Content-Length`, `Connection`, and `Accept-Encoding: identity`; apps may not set those or `Transfer-Encoding`, `TE`, `Upgrade`, `Keep-Alive`, or `Trailer`. Header names are RFC 7230 tokens of at most 64 bytes and values at most 2,048 bytes without control characters. The default `User-Agent` is `ESP-Media-App/<firmware> (<app id>)`; an app may set its own. URLs are at most 2,048 bytes of printable ASCII without user info. Bodies are at most 32 KiB. HTTP/1.1 fixed-length, chunked, and close-delimited bodies are supported; a compressed response is rejected. Up to five redirects are followed when `redirects` is true; 301/302 from a non-GET and 303 switch to GET, and `Authorization` and `Cookie` headers are dropped once a redirect leaves the original scheme, host, and port. One idle connection to the last scheme/host/port is kept for the next request. The host keeps no cookie jar and no cache.

**JSON mode.** With `json`, a 2xx response whose content type contains `json` streams through the host's JSON filter as it arrives (up to 2 MiB on the wire) and only kept values reach Lua as `response.json`. `keep` lists up to 32 object keys (1–64 bytes each) that are kept at any depth under kept parents; array elements and the root are always kept; without `keep` every member is kept. `max_string` (0–65,536; 0 means no cap) cuts each kept string on a UTF-8 boundary and `max_items` (0–10,000) keeps only the first elements of each array; either sets `truncated`. The kept result may hold at most 8,000 values and 128 KiB of text, and nesting of kept containers is limited to 16. A non-2xx or non-JSON reply in JSON mode arrives as `body`, cut to 4 KiB. Without `json`, the body arrives as a string cut to `max_bytes` with `truncated` set. JSON `null` is `esp.json.null`.

**Transport security.** HTTPS validates the server certificate against the Mozilla root bundle that ESP-IDF's mbedTLS component links into the firmware, with the URL's host for SNI and name checks. Validation needs the clock, so HTTPS fails with `Clock not set` until SNTP has synchronized. `http://` is allowed to listed hosts and is unprotected. Serial logs show only method, host, status, timing, and reuse—never paths, queries, headers, or bodies.

### Codecs and helpers

| Operation | Contract |
| --- | --- |
| `esp.json.decode(text [, options])` | Parse up to 32 KiB inside the callback with the same `keep`, `max_string`, and `max_items` options. Return the value and `truncated`, or `nil, error`. |
| `esp.json.encode(value)` | Return JSON text (at most 16 KiB, depth 16), or `nil, error`. A table whose keys are exactly 1…n becomes an array; any other table becomes an object with sorted string keys; an empty table is `{}`. Strings must be UTF-8; NaN, infinity, functions, and mixed keys are refused. |
| `esp.url.encode(text)` / `esp.url.decode(text [, plus_as_space])` | Percent-encoding over the RFC 3986 unreserved set (16 KiB at most). `decode` returns `nil, error` for a malformed escape. |
| `esp.url.query(table)` | `key=value&…` text with sorted, percent-encoded string keys and string, number, or boolean values. |
| `esp.base64.encode(bytes)` / `esp.base64.decode(text)` | Standard alphabet with padding, inputs up to 96 KiB. `decode` returns `nil, error` for invalid input. |
| `esp.hash.sha256(bytes)` / `esp.hash.sha512(bytes)` | Lowercase hex digest of up to 96 KiB. |
| `esp.hash.find_sha512(template, hash_hex, limit)` | Proof-of-work search: the first `i` in `0…limit-1` for which the SHA-512 of `template` with its single `%d` replaced by `i` equals `hash_hex` (128 hex digits), or `nil`. `limit` is at most 200,000; the search is host work outside the 20 ms limit. |
| `esp.text.from_html(html [, max_bytes])` | Plain text of an HTML fragment of up to 64 KiB: tags dropped (script and style content too), `<br>` and block ends become line breaks, entities decoded, spaces collapsed. Returns the text cut on a UTF-8 boundary at `max_bytes` (1–16,384, default 4,096) and `truncated`. |
| `esp.ui.wrap(text, width [, size [, max_lines]])` | Wrap up to 16 KiB of text with the UI font of `size` (0–2, default 1) at `width` pixels (16–640). Returns an array of at most `max_lines` (1–256, default 64) lines—each at most 96 bytes, ready for `drawString`—and whether text remained. |

### Canvas additions

| Operation | Contract |
| --- | --- |
| `esp.ui.text_box(x, y, width, lines, text, color, background [, size])` | Draw up to 96 bytes of text wrapped at word boundaries into 1–8 lines of `width` (16–640) pixels; the last line is ellipsized when text remains. `size` is a `UiText` size, default 1. One draw command. |
| `esp.bitmap.decode(bytes [, background])` | Decode a PNG (non-interlaced, any color type; alpha is blended over the RGB565 `background`, default black) or a baseline JPEG of up to 128 KiB into an RGB565 bitmap of at most 320×432. Return `handle, width, height`, or `nil, error` (`Unsupported image`, `Image too large`, `Too many bitmaps`, `Bitmap memory full`, `Cannot decode image`). An app holds at most 16 bitmaps and 256 KiB of pixels. |
| `esp.bitmap.free(handle)` | Release a bitmap. Frames that still name it skip it. |
| `esp.ui.bitmap(handle, x, y)` | Draw a bitmap; its pixel area counts toward the frame's drawing budget. |

Bitmaps live in PSRAM and are released when the app ends. Decoding runs in the app worker and counts as host work.

### Keyboard

`esp.ui.prompt(title, initial [, options])` accepts `initial` of up to 1,024 bytes and `options = {mask = true}` to hide the typed text; `on_input` receives up to 1,024 bytes. The system keyboard has an EN/RU key: the Russian layout is ЙЦУКЕН with Ё on the digit row and Ъ on the bottom row, and editing moves over whole UTF-8 characters. The chosen layout persists until the device restarts.

`examples/apps/dvach` is a reader for the 2ch imageboard. It opens a board by its exact name from the catalog (`/<board>/catalog.json`, filtered to about 70 KB of text even for a 600 KB catalog), shows six threads per page, reads a thread 25 posts at a time through `/api/mobile/v2/after/<board>/<thread>/<num>`, and replies after the user solves the site's emoji captcha by tapping the keyboard images in the captcha picture; the app finds the posting proof-of-work answer with `esp.hash.find_sha512` and sends the post as multipart form data. It tries the mirrors `2ch.su` (the current main domain), `2ch.hk`, and `2ch.org` in order, moving on when one cannot be reached or answers with a Cloudflare browser check (`cf-mitigated: challenge`), which the device cannot pass; it remembers the mirror that worked and up to eight recent boards in app data. Catalogs and post batches are turned into rows a slice at a time from `on_tick` (25 threads or 4 posts per callback), because converting a whole 250-thread catalog in one callback overran the 20 ms limit on the device, where the Lua heap is in PSRAM. Creating threads (which needs an image) and viewing post images are not supported. 2ch content includes adult boards; the app shows whatever board is typed. `tools/mock_dvach.py` serves a synthetic copy of these endpoints for device tests (see its header), and `tests/lua_dvach_mock/run.sh` runs the app on the host against the same data.

## Future SDK versions

Extend the versioned `esp` API through controller-owned services. LED, Wi-Fi settings, arbitrary file content read/write, file copy, streaming a response to a file, and thumbnails remain future candidates. Built-in browser and assistant operations and the exclusive USB Drive transition need explicit host workflows. Never expose raw GPIO/SPI controllers or protected files as pointers or unrestricted paths; new file operations must validate through `PathPolicy`. Add an example, device verification, and an updated contract here for each version.
