# Assistant dependency probe

This diagnostic is isolated from production navigation and is compiled only
when `ESP_MEDIA_ASSISTANT_DEPENDENCY_PROBE` is defined. It never contacts
Xiaozhi. Build it with:

```sh
tools/build_assistant_dependency_probe.sh
```

Without local configuration the image runs the deterministic 1,000-frame Opus
benchmarks at 16 and 24 kHz and proves that an absent WebSocket trust store is
rejected. The diagnostic uses the production two-port profile, so capture its
115200-baud `Serial` output from the board's USB-to-UART connector. Native USB
CDC remains disabled.

## Local WSS fixture

For the complete TLS and reconnect suite, choose a DNS name or LAN IP that the
ESP32 can use to reach the development computer. The certificate identity and
`AssistantProbeConfig::Host` must be identical.

```sh
tests/assistant_dependency_probe/generate_test_certificates.sh 192.0.2.10
cp tests/assistant_dependency_probe/ProbeConfig.example.h \
  tests/assistant_dependency_probe/ProbeConfig.h
```

Replace the two placeholder PEM blocks in the ignored `ProbeConfig.h` with
`generated/ca.crt` and `generated/wrong-ca.crt`, then start the fixture:

```sh
python3 tests/assistant_dependency_probe/wss_echo_server.py \
  --cert tests/assistant_dependency_probe/generated/server.crt \
  --key tests/assistant_dependency_probe/generated/server.key
```

The ESP32 uses its already-saved Wi-Fi credentials. Rebuild, flash, and monitor
Serial. The probe synchronizes its clock from `pool.ntp.org` or `time.nist.gov`
before the TLS test, so the Wi-Fi network must permit DNS and NTP/UDP 123 as
well as access to the local fixture. The fixture rejects a missing custom
header, acknowledges a valid header with `header-ok`, and echoes text and
binary frames. The diagnostic checks wrong/missing CA rejection, valid-CA
success, the header acknowledgement, 1,000 text plus 1,000 binary round trips,
20 connection cycles, heap recovery, largest internal block, Opus packet/sample
bounds, P95 timings, free PSRAM, and the dedicated 48 KiB probe task's stack
high-water mark. The fixed-point Opus benchmark does not run on Arduino's
smaller `loopTask` stack.

All generated keys and certificates are test-only and ignored by Git. Do not
reuse them for Xiaozhi or any production endpoint.
