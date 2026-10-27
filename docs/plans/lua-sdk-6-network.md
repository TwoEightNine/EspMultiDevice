# Plan: Lua SDK 6 — network requests, response parsing, and a 2ch app

Status: implemented, including the "next iteration" (thread view, posting, and the emoji captcha); the contract of record is [external-apps.md](../external-apps.md). The firmware builds, `tests/lua_net_contract` passes on the host (also against a real 603 KB `/pr/` catalog), and `tests/lua_dvach_mock` runs the app through a full session against synthetic data. Nothing has been exercised on a device yet: TLS against the real mirrors, heap headroom during a handshake, catalog streaming time, and whether 2ch accepts a post from the device all still need a device run. Differences from the proposal below:

- No CA bundle generator was needed: the core's precompiled mbedTLS already links ESP-IDF's full Mozilla bundle, and `HttpClient` attaches it.
- `app-net` uses a 16 KiB stack on core 0 at priority 1, like the Browser workers, rather than 12 KiB on the other core.
- Request bodies may be 32 KiB (2ch comments reach 15,000 bytes), text responses up to 96 KiB, and `text_box` takes 1–8 lines.
- Bitmaps are `esp.bitmap.decode/free` plus `esp.ui.bitmap`, not `esp.image.decode`; the proof-of-work is a host search, `esp.hash.find_sha512`, because a Lua loop would not fit the callback limits; `esp.text.from_html` and `esp.ui.wrap` cover long post text.
- The masked prompt was kept (`{mask = true}`), prompts take 1,024 bytes, and the system keyboard gained an EN/RU layout.
- The live captcha differs from older documentation: the token goes in `emoji_captcha_id` (not `2chcaptcha_id`), and the id response gives the challenge as a Python-style string that the app parses.
- 2ch.su is now 2ch's main domain, so the app tries it first; captcha ids are requested with `board` and `thread` per the site's api.yml, and emoji clicks are sent as `application/json`.
- On the device a 250-thread `/b/` catalog converted in one callback overran the 20 ms limit, so the app converts responses in slices from `on_tick`. Package loads failed at random because battery-history appends hold the SD bus for about 50 ms each second; app SD waits are now 250 ms.
- The callback limit was wall time and the app worker ran at idle priority on core 0, so `app-net`'s TLS handshakes and JSON streaming (priority 1, same core) were charged to Lua callbacks; one `on_tick` "took" 205 ms while running under 1,000 instructions. The worker now runs at priority 2 and the limit counts its own CPU time. Garbage-collection debt from host-built Lua objects is also paid before each callback starts.
- Posts are read through `/api/mobile/v2/after/<board>/<thread>/<num>` 25 at a time (the endpoint returns everything from `num`, so `max_items` bounds what is kept).

Goal, in two parts:

1. An SD-card Lua app can make HTTP(S) `GET`, `POST`, and `PUT` requests and parse the responses (JSON, form/URL encoding, Base64) without blocking the UI or its own callbacks.
2. `examples/apps/dvach` opens a 2ch (Двач) board by its exact name, for example `pr` or `b`, and shows a paged list of its threads. Reading threads, posting, and the captcha are designed below as the next iteration, so that SDK 6 does not rule them out, but they are not part of this delivery.

An earlier draft targeted Reddit. That was dropped because Reddit approves Data API access by hand and may stop accepting new requests altogether.

## What 2ch actually serves (probed 2026-10-26)

| Need | Endpoint | Observed |
| --- | --- | --- |
| Threads of a board, newest bump first | `GET https://2ch.hk/<board>/catalog.json` | 200, `application/json`, no `Content-Length` (chunked), identity encoding, no cookies or headers required, behind Cloudflare. `/pr/` is 603 KB with 204 threads; `/b/` is 413 KB. Top level: `board` (with `name`, `category`, `max_pages`, `threads_per_page`, `enable_posting`, …), `threads[]`, and advert fields. Each thread has `num`, `subject` (up to ~100 characters, plain text, Cyrillic), `comment` (HTML, up to ~10 KB), `posts_count`, `files_count`, `lasthit`, `timestamp`, `sticky`, `closed`, `endless`, `files[]`, and others. |
| Lighter list, ordered by rating | `GET /<board>/threads.json` | 324 KB for `/pr/`. Each thread has `num`, `subject`, `comment`, `posts_count`, `score`, `views`, `lasthit`. |
| One page (20 threads with their last posts) | `GET /<board>/index.json`, then `/<board>/1.json`, … | 155 KB per page. Heavier per thread than the catalog. |
| One thread | `GET /<board>/res/<num>.json` | Next iteration. |
| All boards | `GET /api/mobile/v2/boards` | 374 KB array of `{id, name, category, …}`. Not needed when the user types an exact name. |
| Unknown board | `GET /zzqx/catalog.json` | 404, `text/html` "Not found" page. |
| Mirrors | `2ch.hk`, `2ch.su`, `2ch.org` | All three serve the same JSON. Browser HTML on `2ch.hk` now 301-redirects to `2ch.su`. `2ch.life` returned a Cloudflare 403 challenge page. |

Reading needs no captcha, cookie, or account. The 18+ cookie (`usercode_auth`) that older documents mention was not needed: `/b/catalog.json` returned 200 without it. Captchas appear only for posting. The other failure seen while reading is a Cloudflare challenge: a 403 or 503 HTML page with a `cf-mitigated: challenge` header, as `2ch.life` returned. It needs JavaScript and cannot be solved on the device. The app falls back to the next mirror and otherwise reports it.

### Posting and the captcha (next iteration)

The board page script (`/static/js/board.js`, `/static/js/captcha/EmojiCaptcha.js`) shows the current flow. The old `2chcaptcha` text-image captcha endpoints now return 404 (`code 667`).

1. `GET /api/captcha/emoji/id` returns `{result: 1, type: "emoji", id: <opaque token>, challenge: "{'hash': <SHA-512 hex>, 'limit': 20000, 'template': '…%d…'}"}`.
2. `GET /api/captcha/emoji/show?id=<id>` returns about 72 KB of JSON: `image` (a Base64 PNG, 300×100) and `keyboard` (8 Base64 PNGs of about 50×45 px).
3. The user taps the keyboard emojis that appear in the image. Each tap is `POST /api/captcha/emoji/click` with the JSON body `{"captchaTokenID": <id>, "emojiNumber": <0-7>}`. The answer is either a new `{image, keyboard}` for the next step or `{success: <token>}`.
4. Proof of work: find `i` in `0…limit-1` such that `sha512_hex(template with %d → i) == hash`.
5. `POST /user/posting?nc=1` as `multipart/form-data` with `task=post`, `board`, `thread`, `comment`, optional `subject`/`email`/`name`, `captcha_type=emoji_captcha`, `2chcaptcha_id=<success token>`, and `2ch_challenge=<i>`. The response is `{"result": 1, "num": …}` on success, or `{"result": 0, "error": {"code": …, "message": …}}` with HTTP 200. Codes include `-5` (wrong captcha), `-8` (posting too fast), and `-20` (empty message).

The person using the device solves the captcha by hand; the app only displays it and relays taps. That is the site's normal client flow, not a bypass.

## Constraints found in the current code

| Area | Today | Consequence for SDK 6 |
| --- | --- | --- |
| App worker | One `sd-app` task (16 KiB stack, core 0) runs every callback with a 100,000-instruction / 20 ms limit, then services tones, recording, playback, scans, and jobs | A TLS connect or a slow server must never run inside a callback or that loop. Requests need their own task. |
| Lua heap | 512 KiB PSRAM | A 600 KB catalog cannot become Lua tables. Most of it is HTML comments, file metadata, and adverts. Even the bytes alone do not fit as one Lua string. |
| JSON | `cJSON` from ESP-IDF, used by the manifest and the Assistant | A cJSON tree is thousands of small `malloc` nodes in internal RAM, which TLS and Wi-Fi need. `cJSON_InitHooks` is global, so it cannot be redirected just for apps. |
| `BrowserTransport` | GET/POST only, Browser-specific download store and cookie jar, compressed responses rejected, `setInsecure()` | Not reusable as is. Its no-validation TLS policy is not something to spread to apps. |
| Verified TLS | The Assistant pins one DigiCert root (`XiaozhiTrust.h`); `SecureWebSocketClient` can take a certificate bundle | The trust plumbing exists. A general client needs a CA bundle, because 2ch sits behind Cloudflare and its certificate issuer can change. |
| Handshake memory | `TlsMemoryScope` moves mbedTLS handshake allocations to PSRAM | Reuse it for every app handshake. |
| Canvas text | `drawString` with 96-byte text and no wrapping or ellipsis; only `list_cell` ellipsizes | A thread subject needs a wrapped, ellipsized two-line box. `UiText` already renders Cyrillic. |

## Design decisions

1. **SDK 6 with a `network` permission and a host allowlist.** `esp.net` exists only when the manifest has `"sdk": 6`, `"permissions": ["network"]`, and a `"hosts"` array of 1–8 exact lowercase host names or IP literals, for example `["2ch.hk", "2ch.su", "2ch.org"]`. Every request URL and every redirect target must match one of them. `esp.json`, `esp.url`, and `esp.base64` need no permission. SDK 1–5 apps are unchanged.
2. **One generic HTTP client in `services/network`.** The new `HttpClient` sends one HTTP/1.1 request with a method, headers, and body. It handles `Content-Length` and chunked framing and sends `Accept-Encoding: identity`. It follows up to five redirects only when asked to. It keeps one idle connection to the last scheme, host, and port for reuse, which matters when paging a board on the same host, and streams the response body to a sink interface. It never draws, navigates, or touches SD. `BrowserTransport` stays as it is in this iteration; moving the Browser onto `HttpClient` is a separate follow-up.
3. **TLS certificate validation is on.** App HTTPS validates against a CA bundle. The first spike checks whether Arduino-ESP32 3.3.8's precompiled mbedTLS exposes `esp_crt_bundle_attach` with its built-in Mozilla bundle. If not, `tools/generate_ca_bundle.py` builds a pinned bundle in ESP-IDF's `x509_crt_bundle` format from a recorded Mozilla `certdata` revision, with the revision and licence (MPL-2.0) recorded in `docs/development.md`. Handshakes run inside `TlsMemoryScope`. Validation needs a valid clock, so a request made before SNTP has synchronized fails with `Clock not set`. Plain `http://` is allowed only to hosts the manifest lists. It is for LAN servers and the test mock, and is documented as unprotected.
4. **One network task per open app.** `AppNetWorker` owns an `app-net` task (12 KiB internal stack, created on the first request and deleted when the app ends), a queue of up to four requests, and one result slot per request. It runs on the other core from the `sd-app` worker so a handshake does not delay callbacks. Requests run one at a time in submission order. Cancelling a request closes the socket at the next read. Closing the app cancels everything and waits for the task to exit before `AppRuntime::end()` returns.
5. **Results arrive as a callback.** The `sd-app` loop takes finished results and calls `on_response(id, response)`, as it calls `on_input` today. It waits 10 ms instead of 100 ms while a request is active. Results are held while the app is paused under the keyboard or a viewer, and are delivered after `on_resume()`. Building the response table is bounded host work, so it does not count toward the 20 ms limit, as host file I/O is excluded today.
6. **JSON is parsed by streaming, with a key filter and size caps.** `JsonPick` is a small incremental parser in `features/apps`. It consumes body bytes as the socket delivers them, so a 600 KB catalog is never buffered. It keeps only object members whose key is in an allowlist (array elements and the root are always kept). It caps every kept string at `max_string` bytes on a UTF-8 boundary and every array at `max_items` elements. It writes a compact node array into PSRAM: at most 8,000 nodes, 128 KiB of string bytes, and nesting depth 16. It decodes `\u` escapes and surrogate pairs and rejects invalid UTF-8. The filter is by key name at any depth, not by path, which keeps it small; a key such as `name` that also appears elsewhere costs only a few extra bytes. JSON mode parses only a 2xx response whose content type contains `json`. Anything else, such as 2ch's 404 HTML page, comes back as a `body` string of at most 4 KiB with `json = nil`. `esp.json.decode` runs the same parser inside the callback for small inputs. cJSON is not used for app data.
7. **Bounded everything.** URL up to 2,048 bytes. Up to 16 request headers, names up to 64 bytes (RFC 7230 token characters), values up to 2,048 bytes, with no CR/LF. `Host`, `Content-Length`, `Transfer-Encoding`, `Connection`, and `Accept-Encoding` are set by the host only. Request body up to 16 KiB. A text response delivered as a Lua string is capped at 96 KiB (option `max_bytes`, default 32 KiB); this fits the 72 KB captcha `show` response for the next iteration. A JSON-mode response may stream up to 2 MiB before filtering. Response headers delivered to Lua: up to 32, lowercased, 4 KiB in total. Connect timeout 10 s; overall timeout `timeout_ms` from 1 to 60 s, default 20 s.
8. **Safe logging and no host state.** Serial logs show method, host, status, byte counts, timings, and connection reuse, the same as the Browser logs. They never show the path, query, headers, or bodies. The host keeps no cookie jar and no cache. An app that needs cookies reads `set-cookie` and sends `Cookie` itself.
9. **One small UI addition.** `esp.ui.text_box(x, y, width, lines, text, color, background, size)` is a canvas command that wraps at word boundaries with `UiText::measure` and ellipsizes the last line, for up to 4 lines and 96 bytes of text (the existing command buffer). The host wraps when it draws, so Lua does not measure.

## Proposed API (SDK 6)

| Operation | Contract |
| --- | --- |
| `esp.net.request(options)` | `options`: `method` (`"GET"`, `"POST"`, `"PUT"`, `"PATCH"`, `"DELETE"`), `url`, optional `headers` (string→string table), `body` (string), `timeout_ms`, `max_bytes`, `redirects` (boolean; default `true` for GET, `false` otherwise), and `json = {keep = {key, …}, max_string = n, max_items = n}`. Return a request id (positive integer), or `nil, error` (`Network permission required`, `Host not allowed`, `Invalid URL`, `Invalid header`, `Body too large`, `Too many requests`, `Wi-Fi not connected`). |
| `esp.net.get(url [, options])` | Same as `request` with `method = "GET"`. |
| `esp.net.post(url, body [, options])` | `method = "POST"`. `content-type` defaults to `application/x-www-form-urlencoded`; `options.headers` overrides it. |
| `esp.net.put(url, body [, options])` | `method = "PUT"`, same body rules. |
| `esp.net.status(id)` | `state` (`"queued"`, `"connecting"`, `"sending"`, `"receiving"`), `received`, `total` (−1 when unknown, as with 2ch's chunked responses), or `nil` for an unknown or finished id. |
| `esp.net.cancel([id])` | Cancel one request, or all. A cancelled request still delivers `on_response` with `error = "Cancelled"`. |
| `on_response(id, response)` | `ok` (no transport error and status 200–299), `status` (0 on transport error), `headers`, `url` (after redirects), `body` (text mode, or a non-JSON reply in JSON mode), `json` (JSON mode only), `truncated` (a cap was hit), and `error` (`Timeout`, `Cannot connect`, `TLS failed`, `Clock not set`, `Response too large`, `Invalid JSON`, `JSON too large`, `Redirect not allowed`, `Wi-Fi lost`, `Cancelled`). |
| `esp.json.decode(text [, options])` | Parse up to 32 KiB inside the callback, with the same limits and options. Return a value, or `nil, error`. JSON `null` becomes `esp.json.null`. |
| `esp.json.encode(value)` | A sequence becomes an array, otherwise an object with string keys. Depth up to 16, output up to 16 KiB. Return the text, or `nil, error` for cycles, functions, NaN or infinity, mixed keys, or oversize output. |
| `esp.url.encode(text)` / `esp.url.decode(text)` | Percent-encoding using the RFC 3986 unreserved set. |
| `esp.url.query(table)` | `application/x-www-form-urlencoded` text with keys in sorted order. |
| `esp.base64.encode(bytes)` / `esp.base64.decode(text)` | Standard alphabet with padding, via `mbedtls_base64`, input up to 96 KiB. |
| `esp.ui.text_box(...)` | Wrapped canvas text, as described above. Counts as one of the 64 commands. |

The host sends `User-Agent: ESP-Media-App/<firmware> (<app id>)` unless the app sets its own. 2ch accepted that form of User-Agent during probing.

## 2ch app (`examples/apps/dvach`)

Manifest: `{"id":"dvach","name":"Двач","sdk":6,"app_icon_bg":…,"permissions":["network"],"hosts":["2ch.hk","2ch.su","2ch.org"]}` plus a 64×64 logo from `tools/generate_example_app_logos.py`. Names are UTF-8 and up to 40 bytes, so "Двач" (8 bytes) fits.

**Mirrors.** The app tries `2ch.hk`, then `2ch.su`, then `2ch.org`. It moves to the next mirror on `Cannot connect`, `TLS failed`, `Timeout`, or a Cloudflare challenge (status 403 or 503 with header `cf-mitigated: challenge`). It remembers the last working mirror in `settings.json`. The host allowlist, not the app, decides which hosts are reachable.

**Pages** (canvas, 320×432 below the app bar):
- **Home**: *Open board…* (keyboard prompt) and up to 8 recent boards as list cells, showing `/pr/ — Программирование` and saved in `boards.json`. The most recent board is first. Holding a cell removes it.
- **Board**: title `/<id>/ <board.name>`. Six threads per page in 64 px rows. Each row is a two-line `text_box` with the subject, or when it is empty the start of the opening post's text, and a compact meta line: reply count, file count, a pin marker for `sticky`, a lock marker for `closed`, and the time since `lasthit`. A 48 px footer has previous and next targets and a `3 / 34` page label. While loading, the app shows `esp.ui.progress` driven by `esp.net.status` received bytes, since 2ch sends no length. Back returns Home. Tapping a thread does nothing in this iteration.

**Board loading.** The prompt input is trimmed and lowercased, and surrounding slashes are removed (`/pr/` → `pr`). It must then match `^[a-z0-9]{1,16}$`. The app sends `GET https://<mirror>/<board>/catalog.json` with `json = {keep = {"board","name","threads","num","subject","comment","posts_count","files_count","lasthit","sticky","closed"}, max_string = 200, max_items = 250}`. The `/pr/` catalog then streams 603 KB and keeps roughly 204 × 8 nodes and under 70 KB of strings, inside the host caps. The app turns `comment` into plain text by removing tags, turning `<br>` into spaces, and decoding `&quot; &amp; &lt; &gt; &#NNN;`. It does this a few rows at a time, only for threads whose subject is empty. The whole catalog is held, so paging is local, and a refresh target reloads it. Errors map as follows: 404 shows `No board /<id>/`. A challenge on every mirror shows `2ch is asking for a browser check; try later`. Transport errors show their own text.

Optional `config.json` key `base_urls` (a list) exists only for the mock. The allowlist still applies.

## Next iteration: threads, posting, captcha (not in this delivery)

| App feature | Endpoint | SDK addition it needs |
| --- | --- | --- |
| Thread view | `res/<num>.json` (posts with HTML `comment`) | A host `esp.text.from_html(html, max_bytes)` that reuses the Browser's entity and `<br>`/block handling, so Lua does not strip tags from long posts. It also needs wrapped multi-line text beyond 96 bytes per command, or a text-pager layer. |
| Captcha display | `emoji/id`, `emoji/show` (Base64 PNGs) | `esp.image.decode(bytes)` → handle, decoded by the host (PNGdec) into PSRAM RGB565: up to 16 handles, 320×432 each, 256 KiB of pixels in total. A canvas command `esp.ui.image(handle, x, y)`. The 8 keyboard emojis become tap targets on the canvas. |
| Captcha taps | `POST emoji/click` with a JSON body | Covered by SDK 6 (`esp.json.encode`, `esp.net.post` with a `content-type` override). |
| Proof of work | SHA-512 search up to `limit` (20,000) | `esp.hash.sha512(text)` returning hex. The app checks about 1,000 candidates per callback across ticks, which takes 2 s at worst. A host-side search job is the fallback if that proves too slow. |
| Posting | `POST /user/posting?nc=1`, `multipart/form-data` | `esp.net.request{multipart = {{name=…, value=…}, …}}` with text parts only (no files) and a host-generated boundary. |
| Writing Russian | comment text | The system keyboard is Latin-only today, so it needs a Cyrillic layout in `TextInputScreen` and a larger prompt limit than 255 bytes. |
| Rate limits | posting errors `-8` and `-22` | App-side cooldown. |

## Work phases

1. **Spikes** (decide before writing the API). (a) CA bundle availability in 3.3.8 and, if one must be generated, its size; plus the certificate chain 2ch's mirrors present. (b) Free internal heap with a 512 KiB Lua app open plus an `app-net` handshake to `2ch.hk` and a reused second request; the target is at least 30 KiB left, with the Browser and Assistant closed. (c) Wall time to stream and filter the 603 KB `/pr/` catalog on the device. The target is under 3 s on a good Wi-Fi link; if it is much slower, fall back to `threads.json` or paged `index.json`.
2. **`HttpClient` + trust** in `src/services/network/`: request and response framing, chunked decoding, redirects with an allowlist callback, the `Authorization` header dropped on a cross-host redirect, connection reuse, cancellation, timeouts, safe logs, and a body sink interface.
3. **Codecs**: `JsonPick` (streaming filter, string and array caps, node array), JSON encode, URL, and Base64. Add a host-compiled contract test in `tests/lua_net_contract/` covering escapes, surrogates, invalid UTF-8, depth, node and byte limits, the filter, `max_string` cuts on UTF-8 boundaries in Cyrillic text, `max_items`, split-buffer feeding at every byte boundary, a recorded `/pr/catalog.json` fixture, and encode edge cases.
4. **Runtime wiring**: `AppPackage` parses `network` and `hosts` and accepts SDK 6. `AppNetWorker` and the `esp.net` / `esp.json` / `esp.url` / `esp.base64` registration go in `AppRuntime`, with `on_response` delivery, hold-while-paused, and teardown. `WiFiController` is passed through `AppHostScreen` from `esp_media.ino`, for the connected check only. Add the `text_box` command in `AppRuntime::luaDraw` and `AppHostScreen::drawFrame`.
5. **2ch app and mock**: `examples/apps/dvach/{manifest.json,main.lua,logo.png}`. Add `tools/mock_dvach.py`, a stdlib-only HTTP server serving recorded fixtures: a full catalog, a board with empty subjects and HTML-only OP text, sticky and closed threads, a 404 HTML page, a Cloudflare-style challenge on one mirror path, and a slow chunked response.
6. **Docs**: an SDK 6 section in `docs/external-apps.md` covering the permission, allowlist, limits, TLS stance, and an example. `docs/services.md` gets `HttpClient` and trust. `docs/architecture.md` gets the `app-net` task and result hand-off. `docs/development.md` gets the CA bundle source, licence, fixtures, and the mock tool. `docs/ui-kit.md` gets `text_box`. README gets a short summary.

## Verification

- Firmware builds with `tools/build_esp32s3.sh` after each phase. `tests/lua_net_contract` passes on the host.
- On device, against the real site: open `pr`, `b`, and `po`, page through all of `/pr/`, use refresh, try an unknown board, and confirm the Cyrillic subjects render correctly. Then turn Wi-Fi off mid-request, open the keyboard while a catalog is loading (the result must arrive after resume), and close the app mid-stream. The task must exit and the heap must return to its baseline in the `[Navigator] release` line.
- On device with the mock (a manifest copy that adds the mock's LAN IP to `hosts`): mirror fallback on a challenge, empty-subject rows, and cancel during a slow response.
- TLS: a host whose certificate does not chain to the bundle (for example `https://self-signed.badssl.com`, added to a test manifest) must fail with `TLS failed`. A request made before SNTP has synchronized must fail with `Clock not set`.
- Policy: no `esp.net` without the permission or on SDK ≤ 5. Unlisted hosts and unlisted redirect targets are refused. CR/LF in a header is refused, as are reserved headers. Serial logs contain no path or query.
- Touch stays responsive while a catalog streams. SDK 1–5 examples still run.

## Notes to document

- 2ch content includes NSFW boards. The app shows whatever board the user types and does no filtering.
- Certificate validation protects the traffic only for `https://` hosts; `http://` allowlist entries are unprotected.
- No credentials are involved in reading. Posting in the next iteration will store only short-lived captcha tokens, in memory.

## Later

Thread view, posting, and the emoji captcha (see above), a browse-all-boards page from `/api/mobile/v2/boards`, thumbnails from `files[].thumbnail`, migrating `BrowserTransport` onto `HttpClient`, and streaming a response to an app-data file for large downloads.
