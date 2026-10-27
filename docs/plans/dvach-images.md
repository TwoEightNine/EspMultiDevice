# Plan: images in the 2ch app

Status: implemented; the SDK contract of record is [external-apps.md](../external-apps.md). It extends [lua-sdk-6-network.md](lua-sdk-6-network.md). The firmware builds, and on the host `tests/bitmap_scale` and `tests/lua_dvach_mock` (thumbnails, downloads, reopening, cancel, a refused mislabeled file, the toggle) pass. Nothing has been run on a device yet, so the device checks under Verification are still open. Differences from the proposal below:

- The folder is `/Downloads/2ch/`: the manifest's display name is "2ch", not "Двач" (that is only the page title). The rule is unchanged: display name, or the app id when the name is not a valid path segment.
- The catalog filter cannot keep only the first file per thread, because `max_items` applies to every array and would also cut the thread list. It keeps `files` with only `thumbnail`, `type`, and `name` per file, which adds about 20 KB of text to a 250-thread catalog.
- Thumbnails stay in the 16-entry LRU across board and thread, so going back to the board does not refetch them; they are freed when leaving the board for Home, opening another board, or turning thumbnails off.
- With thumbnails off the board returns to six compact rows a page, and a thread shows tappable file placeholders (the extension in a dark cell), so files still open.
- A download's `timeout_ms` covers the connect and headers and then each silence while the body arrives (`HttpClient::Request::idleTimeoutMs`), so a 30 MB file is not cut off by the total request time.
- `esp.downloads.stat` returns `nil, error` like `esp.fs.stat`, and its entry table has the `esp.fs` fields.

Goal: `examples/apps/dvach` shows 100×100 thumbnails next to threads on the board page and under posts in a thread. Tapping one downloads the full file into `/Downloads/<app name>/` (for this app `/Downloads/2ch/`), the way the Browser saves downloads, and opens it in the built-in Image Viewer. MP4 follows the same path into the Video Player when it can decode it; WebM is reported as unsupported.

## What 2ch serves (probed 2026-10-27 on 2ch.su)

Every thread in `catalog.json` and every post from the posts API carries a `files` array. Each file has `name`, `path` (`/b/src/<thread>/<name>`), `thumbnail` (`/b/thumb/<thread>/<id>s.jpg`), `type`, `width`, `height`, `tn_width`, `tn_height`, and `size` in KB.

| Item | Observed |
| --- | --- |
| Thumbnails | Always JPEG, baseline (18 of 18 sampled), at most 250 px on the long side, average 5.7 KB and at most 13 KB. Same host as the JSON. A thread that moved to the archive answers 301 to `/b/arch/thumb/…` on the same host. |
| File types on `/b/` | `type` 1 JPEG (150), 2 PNG (106), 10 MP4 (65), 9 WebP (20), 6 WebM (4), and 4 GIF in threads. |
| Full files | Up to 38 MB and 10,168 px wide. A 516×595 JPEG was 112 KB. |

## What the SDK can't do yet

| Need | SDK 6 today | Gap |
| --- | --- | --- |
| Show a thumbnail in a 64 px row | `esp.bitmap.decode` keeps the source size; a 216×250 thumbnail is 108 KB of RGB565. The budget is 16 bitmaps and 256 KiB. | Two thumbnails exhaust it. Images must be scaled down while decoding. |
| Fetch a thumbnail | `esp.net.get` text mode returns the bytes as a Lua string; Lua then calls `esp.bitmap.decode`. | It works, but each thumbnail costs Lua heap, a copy, and a decode on the app worker. Decoding in the network task is cheaper. |
| Show a full image | `esp.bitmap` caps the source at 128 KiB and the size at 320×432. Saving to SD and opening a viewer need the `files` permission and a general SD path. | Full files must stream into a downloads folder and open in the built-in viewer, without giving the app general SD access. |

## Design decisions

1. **Scaled bitmaps.** `esp.bitmap.decode(bytes, {max_width, max_height, background})` fits the image inside the box with its aspect ratio kept, and never scales up. For JPEG the host picks the largest JPEGDEC decode scale (1/2, 1/4 or 1/8) that still covers the box, so a 216×250 thumbnail fitted to 100×100 decodes at 1/2 size before resampling. PNG is reduced row by row with an area filter. The resampler goes in a small Arduino-free helper (`BitmapScale`) with a host contract test.
2. **Responses that become bitmaps.** A request option `bitmap = {max_width, max_height, background}` decodes a 2xx image response in the `app-net` task into a PSRAM pixel buffer. The app worker registers it as a bitmap when it delivers `on_response`, which gains `response.bitmap`, `response.width`, and `response.height`. The image bytes never become a Lua string. If the bitmap budget is full or the image is unsupported, `error` says so.
3. **Bigger bitmap budget.** 32 handles and 512 KiB of pixels. A 100×100 thumbnail is 20 KB, so the 16 thumbnails the app keeps (about 320 KB) fit with room to spare. About 6.8 MB of PSRAM is free while an app runs, so this is cheap.
4. **One shared download transaction.** The file side of `BrowserDownloadStore` moves to `src/services/storage/DownloadStore`, parameterised by its target folder. That covers the temporary file, 4 KiB writes per `SpiBusLock` hold, the size cap, signature verification through `FileFormats` (including the MP4/3GP ISO-BMFF check), the atomic rename, and discarding on failure. The Browser keeps `/Downloads` and its own name and content-type logic; apps get their own folder. One implementation keeps the verification rules the same for both.
5. **App downloads folder.** Each app downloads into `/Downloads/<app name>/`, using the manifest's display name. If that name is not a valid path segment under `PathPolicy` (for example it contains `/` or a trailing dot), the app id is used instead. The host creates the folder on first use. The app never names a path, so this needs no `files` permission. Files persist until the user deletes them in Files, as with Browser downloads.
6. **Download requests.** A request option `download = "name"` (1–96 bytes, a plain file name with an openable media extension) streams a 2xx body through `DownloadStore` into the app's downloads folder. The cap is 32 MiB: larger than the Browser's 16 MiB because 2ch videos reach 38 MB. The response reports `downloaded` (the file name actually used; an existing name gets a numbered suffix, as in the Browser) and `bytes`. A body whose signature does not match its extension, or a compressed, truncated, or oversized body, is discarded with an error. `esp.net.status` reports progress as for any request.
7. **The downloads API.** `esp.downloads.stat(name)` returns `{size, modified, kind}` or `nil`; the app checks it before downloading, so a file it already has opens at once. `esp.downloads.open(name)` works like `esp.fs.open` but only inside the app's downloads folder. It covers the app with the Image Viewer (JPEG, PNG, GIF, WebP, animated included), the Video Player (MP4/3GP), or the Audio Player, and calls `on_resume()` on return. It needs no `files` permission. Apps that do hold `files` can also reach the folder through `esp.fs` as usual.
8. **Version.** SDK 6 is not committed or released yet, so these additions extend SDK 6 rather than starting SDK 7.

## App changes (`examples/apps/dvach`)

- **Thumbnail size.** One constant, `THUMB = 100`, sets the box that thumbnails are fitted into on both pages, and the layout is derived from it, so the size can change later without touching the rest.
- **Board page.** Rows grow to `THUMB + 8` = 108 px, so three rows fit above the footer instead of six. Each row has its thumbnail in the left 100×100 cell, centered if its aspect ratio leaves margins. The title box (up to four lines) and the meta line take the remaining 196 px. Rows without a file keep the full width. Video files get a small ▶ mark drawn over the thumbnail. The catalog filter adds `files`, `thumbnail`, `type`, `path`, `size`, `width`, `height`, and `name`, with `max_items` keeping only the first file per thread for the list.
- **Thread page.** A post with files gets a strip of up to three 100×100 thumbnails after its text (three fit across 320 px). The strip takes five 21 px line slots, so the page model becomes a list of rows, each either a text line or a file strip. Tapping a thumbnail opens that file.
- **Thumbnail loader.** A queue of thumbnail URLs for the current page, visible rows first, sent one request at a time over the kept-alive connection. Expect roughly 150–300 ms each, so under a second for the three rows of a board page. Changing page cancels the rest. An LRU of up to 16 decoded thumbnails (about 320 KB) is keyed by the thumbnail path and freed when leaving the board or thread. Each slot shows a dark placeholder until its bitmap arrives.
- **Opening a file.** The app uses the file's 2ch name (for example `17686951847150.jpg`), which is unique per file. If `esp.downloads.stat` finds it already, the app opens it at once. Otherwise a progress page shows bytes received and the file size from the post, with a Cancel button. The `download` request saves the file to `/Downloads/2ch/`, then `esp.downloads.open` shows it in the Image Viewer, and Back returns to the same thread position. MP4 downloads the same way and opens in the Video Player; files over about 20 MB first ask whether to continue. WebM shows "WebM can't be played on this device" and downloads nothing.
- **Thumbnails toggle.** A Home setting turns thumbnails off, to save bandwidth or avoid adult images on boards such as `/b/`. The default is on.

## Work phases

1. **SDK:** `BitmapScale` plus scaled decode for `esp.bitmap.decode` and JPEG pre-scaling, with a host test. Raise the bitmap limits.
2. **SDK:** the `bitmap` response mode in `AppNetWorker` (decode in the network task, register on the app worker).
3. **SDK:** extract `DownloadStore` from `BrowserDownloadStore`, leaving the Browser's behavior unchanged, and verify a Browser download still works. Then add the `download` request mode, the per-app downloads folder, and `esp.downloads.stat/open`. Opening reuses the SDK 4 navigation path, with pause, cover, and `on_resume`.
4. **App:** thumbnails on the board page, then thread file strips, then downloading and opening full files.
5. **Tests and tools:** `tools/mock_dvach.py` gains `files` entries, serves thumbnail images and full files (including a large one, to test cancelling, and one whose content does not match its extension), and a WebM entry. The harness learns `bitmap` and `download` responses and `esp.downloads.stat/open`, and checks that the app never holds more than 16 thumbnails.
6. **Docs:** an SDK 6 additions section in `external-apps.md`, the shared `DownloadStore` in `browser.md` and `architecture.md`, the per-app download folders in `media.md`, and the app paragraph.

## Verification

- Host: the scaler contract test (aspect ratio, never upscaling, 1/8 pre-scale, odd sizes), the harness session with thumbnails and an opened image, and the real `/b/` catalog keeping thumbnails within the bitmap budget.
- Device: a `/b/` board page fills its three thumbnails within about a second, and paging quickly cancels pending thumbnails without errors. A large PNG, an animated GIF, a WebP, and an MP4 download into `/Downloads/2ch/`, show up in Files, open, and return to the same place. Opening the same image again skips the download. Cancelling a 30 MB download leaves no `.tmp` file. A Browser download still lands in `/Downloads` and verifies as before. Turning thumbnails off stops all image requests. The heap and bitmap budget return to baseline after leaving a thread.

## Decisions taken

- The folder uses the display name, so this app's files land in `/Downloads/2ch/`. The alternative was the ASCII id, `/Downloads/dvach/`.
- Downloads are never pruned automatically, matching the Browser; the user deletes them in Files.
- Thumbnails default to on, with the Home toggle.
- MP4 is included; WebM stays unsupported because the firmware has no VP8/VP9 decoder.
