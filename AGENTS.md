# Agent guide

ESP Media is an Arduino firmware project for an ESP32-S3 media device. Treat the following documents as the source of truth; update the relevant document whenever behavior, a persistent format, wiring, or a public component contract changes.

## Start here

1. Read [README.md](README.md) for scope, build prerequisites, and repository layout.
2. Read [docs/architecture.md](docs/architecture.md) before changing screen flow, controller ownership, tasks, SPI access, or screen payloads.
3. Read [docs/ui-kit.md](docs/ui-kit.md) before changing any screen or shared UI drawing.
4. Read the focused document for the subsystem being changed:
   - [docs/media.md](docs/media.md) for images, audio/video formats, caches, decoders, or files opened/downloaded as media.
   - [docs/browser.md](docs/browser.md) for HTTP(S), HTML/CSS rendering, cookies, browser persistence, or downloads.
   - [docs/services.md](docs/services.md) for SD hot-swap, secret storage, Wi-Fi, time, battery, recording, playback, or workouts.
   - [docs/hardware.md](docs/hardware.md) for pins, TFT_eSPI, power, wiring, calibration, or SD preparation.
   - [docs/development.md](docs/development.md) for build dependencies, generated assets, fixtures, licenses, or vendor code.
   - [docs/agentic-commands.md](docs/agentic-commands.md) before adding or changing an Assistant device command or backend tool.

## Mandatory UI contract

- Use `UiKit` for app bars, navigation hit testing, shared icons, list cells, and shared progress controls. Add a reusable component to `UiKit` instead of duplicating drawing primitives in a screen.
- Use TFT_eSPI antialiased primitives for icons, circles, rounded rectangles, and button fills. Supply the immediate background color to every smooth primitive.
- Use a 48px app bar with the kit’s 24px icon in its 48×48px hit target; do not hand-position a replacement icon or title.
- Keep icon-only interactive targets equal to their whole visual cells. List actions are 40×40px with 16×16px kit icons and 12px padding.
- Use fill-only smooth rounded buttons and `UiText::Size::Content` labels unless [docs/ui-kit.md](docs/ui-kit.md) documents a screen-specific exception.

## Implementation rules

- Keep `esp_media.ino` limited to hardware composition, screen registration, and the generic main loop. A screen owns its drawing, touch handling, and non-blocking `tick()` work.
- Use `Navigator` payload/result envelopes for screen-to-screen data; do not couple the sketch or unrelated screens to a concrete screen implementation.
- The SD card and touch controller share SPI. Any additional concurrent SD work must use `SpiBusLock` per transaction and leave time for touch polling.
- Use `FileFormats` as the sole registry for supported media extensions, MIME types, signatures, and internal cache formats. Never make `.em16`, `.emga`, or `.empa` source media.
- Preserve cache validation and atomic `.tmp`-then-rename writes. Cache metadata must continue to bind to the source file’s size and last-modified timestamp.
- Keep network, media conversion, decoding, and filesystem work incremental or worker-backed; do not block the UI loop.
- Extend Assistant capabilities only through the documented authenticated `device_command` / `device_result` protocol. New commands require strict schemas on both peers, bounded fields, session/request correlation, safe logging, and a controller-owned UI action; never let a backend command or worker navigate directly.
- Do not edit vendored decoder code unless the task explicitly requires it. Keep changes in project adapters/shims where possible, preserve notices, and document version/license implications.
- Do not present `SecretStorage` or browser HTTPS as secure storage. Both have deliberate, documented security limitations.

## Verification and documentation

- Verify a build after C++ or configuration changes when the local Arduino environment is available.
- For media changes, exercise a representative supported file and any relevant cache invalidation path. Regenerate only the relevant fixture or generated asset with the documented tool.
- For hardware changes, keep the constants in `esp_media.ino`, `tft_setup.h`, pin tables, and affected wiring prose consistent.
- Do not remove unrelated work in a dirty tree. Document material behavior changes in the focused `docs/*.md` file and keep README summaries concise.
