# Pinned video dependencies

- `minimp4`: `5a212a18dba7dca09543bbc7d65619274fd2931a` from
  <https://github.com/lieff/minimp4> (CC0-1.0).
- `lib264`: `ca1f8e0ef1f8cca436cc721f4f079f4606001c52` from
  <https://github.com/ittiam-systems/lib264> (BSD-3-Clause-Clear, including
  its AVC patent notice). The generic C decoder/common sources are compiled
  by project adapters in `src/vendor_shims/`. ARM/x86 SIMD backends are excluded; the adapter
  uses lib264's memory-record API to keep decoder buffers in PSRAM and a
  single-core synchronization shim.
- `libhelix`: `7d0444cac095e2695d63c40ba62b3c40a39ae592` from
  <https://github.com/pschatzmann/arduino-libhelix>. Its wrapper is GPL-3.0
  and the AAC core retains the included RPSL/RCSL notices.

Only the MP4 demux, H.264 decode, and AAC decode portions needed by
`VideoPlayerScreen` are compiled by the project adapter sources in `src/vendor_shims/`.
