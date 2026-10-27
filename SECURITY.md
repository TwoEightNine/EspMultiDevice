# Security policy

## Reporting a vulnerability

Please report security problems privately through GitHub's
[private vulnerability reporting](https://github.com/TwoEightNine/EspMultiDevice/security/advisories/new)
instead of opening a public issue. Include the firmware version from
Settings → About, how to reproduce the problem, and what an attacker could do
with it. ESP Media is a hobby project maintained in spare time, so there is no
guaranteed response time, but every report will be read.

Only the latest version on the default branch receives fixes.

## Known limitations

ESP Media is a personal media device, not a hardened security product. The
limits below are deliberate and documented. Reports that only restate them
are not vulnerabilities, but ideas for improving them are welcome as issues.

- **Hidden files are obscured, not encrypted.** The PIN-protected *hidden
  files* area (`SecretStorage` in code) uses a 4-digit PIN to select an offset
  into a key file stored on the same SD card, then XOR-transforms names and
  contents. Anyone with the card can recover the files. See
  [docs/services.md](docs/services.md).
- **The SD card is plaintext.** Browser cookies, recents, favorites, cached
  pages, downloads, battery history, app data, and media caches are stored
  unencrypted. USB Drive mode exposes the entire card to the connected
  computer.
- **Device storage is not protected.** Normal builds enable neither ESP32
  flash encryption nor secure boot, so saved Wi-Fi credentials and other NVS
  settings can be read by anyone with physical access, and the firmware can
  be replaced over USB.
- **The Browser is a lightweight reader, not a secure browser.** It validates
  HTTPS certificates against the Mozilla root bundle linked into
  Arduino-ESP32, but it has no sandboxing, no site isolation, and only a
  minimal HTML/CSS engine. Do not use it for sensitive accounts. See
  [docs/browser.md](docs/browser.md).
- **The Wi-Fi speed test does not validate certificates.** It sends only
  synthetic data, but a network attacker can falsify the displayed result.
- **SD apps are trusted code.** Lua apps run with execution, memory, path, and
  host limits, but each app declares its own `files` and `network`
  permissions in its manifest and the device does not ask the user to approve
  them. Install only apps from sources you trust. See
  [docs/external-apps.md](docs/external-apps.md).
- **The Assistant depends on your own backend.** It talks only to the
  self-hosted server configured at build time and pins that server's CA.
  Securing that server, its provider API keys, and its stored conversation
  summaries is the operator's responsibility. See
  [tools/xiaozhi_local/README.md](tools/xiaozhi_local/README.md).
