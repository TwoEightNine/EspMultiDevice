# Browser behavior and limits

The Browser is a bounded compatibility-oriented HTTP/HTTPS reader, not a general-purpose secure browser. It needs Wi-Fi but can run without an SD card; persistence is simply unavailable in that case.

## Navigation and persistence

The Home Browser app holds up to 16 recents and 16 favorites, each with URL and title. A URL without a scheme becomes `https://`. The start page lists recents and has icon-only controls for add URL, recent/favorite mode, and eight-entry paging. Reopening a recent promotes it and refreshes its title after the page loads. Links, history navigation, and redirects do not modify recents.

Open pages have back, home, page-up, and page-down controls. The three-dot menu toggles the favorite state and swaps between `ESP-Media-Browser/1.0` and an Android mobile user agent; the choice reloads the current page and resets on Browser exit. While a page or previews load, Back becomes an X that cancels work. The Home control is disabled at the start page.

History is limited to the current session. Cached pages restore immediately when available; fresh URL opens, recents, links, and redirects remain network-first. Session storage is `/.system/browser_session/`, capped at 16 MiB across pages and inline images, and evicts image sources before page bodies. It is cleared on Browser close and cleaned at next startup after an interrupted session. Recents, favorites, and cookies are plaintext in `/.system/browser_state.txt`. The internal `BrowserPersistentState` component owns this bounded model and its atomic temporary-file commit; it does not make the file secure or change its format.

## HTTP(S), redirects, and downloads

The browser supports uncompressed HTTP/HTTPS GET, fixed-length or chunked bodies, up to five redirects, and a bounded cookie jar. It resolves absolute, protocol-relative, root-relative, directory-relative, and query-relative URLs. Fragment-only links and non-web schemes (`mailto:`, `tel:`, `javascript:`) are not tappable.

Text response retention is capped at 512 KiB; longer responses render the retained prefix. Connection timeout is 10 seconds. Page/media transfers show a phase and progress bar; length is a percentage only with `Content-Length`, otherwise a received-byte count.

Direct BMP, PNG, JPEG, GIF, WebP, WAV, MP3, MP4, and 3GP responses stream to `/Downloads/` and open in the appropriate player. Downloads are capped at 16 MiB, use a temporary file, verify the final signature, and are discarded when malformed, compressed, interrupted, or too large. MP4/3GP downloads additionally validate their ISO-BMFF identity. Downloaded files persist until removed outside the browser.

TLS certificate validation is deliberately disabled for compatibility. Do not use the browser for private information or sensitive accounts.

The internal `BrowserTransport` component owns the shared HTTP and HTTPS
clients and serializes foreground navigation with preview transfers. UI code
continues to own progress display and cancellation affordances.

## Text, HTML, and CSS scope

The renderer produces paged readable rows at 22px leading on a white document background; its chrome stays dark. It supports direct Latin and Cyrillic glyphs, including Ukrainian. `BrowserFreeSans.h` is generated from GNU FreeSans by `tools/generate_browser_freesans_font.py`; see `LICENSE.FreeFont`.

Text is normalized to UTF-8 using BOM, HTTP charset, HTML meta charset, UTF-8 validation, and then Windows-1251 fallback. Supported encodings are UTF-8/ASCII, Windows-1251, KOI8-R/U, ISO-8859-5, CP866, ISO-8859-1, and Windows-1252. Explicit unsupported encodings show an explanatory page.

Inline HTML colors and `bgcolor`, plus inline/embedded CSS `color`, `background`/`background-color`, `font-weight`, `font-style`, and underline are supported. Up to 32 embedded rules may use simple element, `.class`, or `#id` selectors, including comma lists; normal specificity/source order and inline style priority apply. External stylesheets, combinators, pseudo-classes, attribute selectors, media queries, CSS variables, malformed rules, and unsupported declarations are ignored. `display: block` and `width: 100%` apply only to image placeholders.

Unsupported images show compact icon-and-alt placeholders; inside HTTP(S) links they are blue and tappable. `audio`, `video`, and `source` tags with HTTP(S) `src` are tappable media links. JavaScript-only pages explain their limitation; embeds remain text stubs. HTTP Basic authentication and form login are not supported.

## Embedded images and forms

Baseline JPEG, non-interlaced PNG, and GIF previews begin after static text renders. A second-core worker downloads/decodes each source and hands results to the UI task. Each source is limited to 384 KiB, renders aspect-fit up to 200 × 200, and is never upscaled; GIF uses its first frame. Cached source URLs are reused on later pages. If a thumbnail allocation cannot fit, remaining sources stay placeholders.

Forms are capped at 8 forms, 32 controls, and 80 select options per document. The supported interactive controls are text/search/email/url/number fields, textareas, checkboxes, radios, single selects, submit, and reset. Named successful values use `application/x-www-form-urlencoded`: GET adds them to the action URL and POST sends an 8 KiB-capped body. POST values are not logged. 301/302/303 redirects become GET; 307/308 preserve the body. Other methods/encodings and password, file, multi-select, date/time, range, color, image, or unknown controls remain unsupported stubs.
