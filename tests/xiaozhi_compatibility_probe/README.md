# Xiaozhi hosted compatibility probe

This compile-time-only Stage 1 diagnostic contacts the official Xiaozhi
bootstrap endpoint, supports the normal account activation flow, establishes a
credentialed WSS connection, exchanges protocol `hello`, and then disconnects.
It does not stream audio, enable MCP, act on server commands, or install any
firmware offered by the bootstrap response.

Build with:

```sh
tools/build_xiaozhi_compatibility_probe.sh
```

Upload the generated image from
`/private/tmp/esp-media-xiaozhi-probe-build` with the Arduino IDE or the same
`arduino-cli` and board port used for normal ESP Media firmware.

The probe uses the same two-port USB profile as production. Monitor UART0 on
the board's USB-to-UART connector at 115200 baud. It uses the Wi-Fi credentials
already saved by normal ESP Media firmware and requires DNS plus NTP/UDP 123
access before its certificate-checked HTTPS request can run.

On an unbound device, the Serial log prints the activation message and code.
Sign in to <https://xiaozhi.me/>, add the device using that code, and leave the
probe running. It polls the upstream activation endpoint without asking for or
storing account credentials.

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
