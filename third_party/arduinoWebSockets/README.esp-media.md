# ESP Media import of arduinoWebSockets

This is the synchronous client/base subset of arduinoWebSockets 2.7.2. The
release source and archive hash are recorded in `VERSION.txt`. The upstream
LGPL-2.1 license is retained as `LICENSE`; libb64's authorship and public-domain
dedication are retained under `src/libb64/`.

The import deliberately excludes the server, Socket.IO, examples, tests, and
asynchronous transports. Arduino compiles the two retained implementation files
only through `src/vendor_shims/websockets/`.

Project code must use `SecureWebSocketClient`, not `WebSocketsClient` directly.
The project adapter is a bounded RFC 6455 client over Arduino-ESP32's
`NetworkClientSecure`; it does not call this library at runtime. It requires a
non-empty PEM CA certificate or ESP certificate bundle. Do not call upstream
`beginSSL()` because its null-fingerprint/null-CA path can become an insecure
compatibility mode.

ESP Media carries narrowly scoped ESP32 source-level evaluation patches. The
constructor initializes the `SSL_AXTLS` client-certificate and key pointers to
null so CA-only setup cannot read uninitialized mutual-TLS pointers. The source
also documents the Arduino-ESP32 3.3.x post-upgrade secure-client issue found
during the Stage 0 fixture work. Despite those changes, the upstream client was
not certified for the project's runtime WSS path; the direct project adapter
passed the full local trust/header/echo/reconnect suite instead. These patches
do not change the pinned upstream archive or its recorded SHA-256.
