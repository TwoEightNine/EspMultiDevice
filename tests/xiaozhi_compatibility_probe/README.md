# Xiaozhi compatibility probe

This compile-time-only Stage 1 diagnostic contacts either the official Xiaozhi
bootstrap endpoint or a build-local self-hosted endpoint, supports activation
when the selected service requires it, establishes a credentialed WSS
connection, exchanges protocol `hello`, and then disconnects.
It does not stream audio, enable MCP, act on server commands, or install any
firmware offered by the bootstrap response.

Build with:

```sh
tools/build_xiaozhi_compatibility_probe.sh
```

Without local configuration the build targets the official hosted service and
trusts its bundled DigiCert root. To use the local backend, copy
`LocalBackendConfig.example.h` to the ignored `LocalBackendConfig.h`, set the
HTTPS URLs, and paste the Caddy root PEM created by
`tools/xiaozhi_local/`. The same root is required for both bootstrap and WSS;
plaintext endpoints and empty trust are rejected.

Upload the generated image from `$TMPDIR/esp-media-xiaozhi-probe-build`
(or `ESP_MEDIA_XIAOZHI_PROBE_BUILD_DIR`) with the Arduino IDE or the same
`arduino-cli` and board port used for normal ESP Media firmware.

The probe uses the same two-port USB profile as production. Monitor UART0 on
the board's USB-to-UART connector at 115200 baud. It uses the Wi-Fi credentials
already saved by normal ESP Media firmware and requires DNS plus NTP/UDP 123
access before its certificate-checked HTTPS request can run.

On an unbound hosted device, the Serial log prints the activation message and
code. Sign in to <https://xiaozhi.me/>, add the device using that code, and
leave the probe running. It polls the upstream activation endpoint without
asking for or storing account credentials.

After binding, the probe retries a missing URL or token through eight total
bootstrap attempts. A placeholder/test token is not retried and is reported by
category without printing its value. This distinguishes delayed provisioning
from the hosted service deliberately withholding production credentials.

After the first valid server `hello`, the probe stores a temporary phase byte
and accepted MCP-advertisement mode, disconnects, and restarts itself. The
second boot reloads the same MAC-derived device ID and NVS UUID, obtains fresh
bootstrap credentials, and requires another server `hello`. A complete run ends
with `STAGE1_PROBE PASS`; the temporary phase keys are then removed. The stable
`assistant/client_uuid` key remains until NVS is erased.

The raw bootstrap response, WSS URL, token, Authorization value, and server
session ID are never printed. The token is not persisted. The bootstrap body
does include the upstream-style board report, including Wi-Fi SSID, RSSI,
channel, and local IP, solely for this compatibility test.

The self-hosted hardware result recorded on 2026-09-15 passed both automatic
boots with target `local-self-hosted`. The first and second boots loaded the
same MAC-derived device ID and NVS UUID, received non-placeholder
credentials, accepted WSS/server hello with MCP disabled, and refreshed the
credential after reboot. The 48 KiB probe task retained 21,076 bytes of stack;
after each closed WSS session the largest free internal block was 90,100 bytes.
