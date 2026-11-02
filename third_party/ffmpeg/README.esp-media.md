# FFmpeg legacy-video subset

This directory is a deliberately small source closure from **FFmpeg 9.0.1**
(`ffmpeg-9.0.1.tar.xz`, SHA-256
`cf38e0e28c7e5605942c4a77755349b0145804a397af37eb1fb4c77cb237f635`).

It was configured for the ESP32-S3's generic-C path with only these decoders:

- `h263`
- `mpeg4` (MPEG-4 Part 2)

It has no FFmpeg programs, encoders, demuxers, muxers, filters, network code,
assembly, or GPL-enabled components. The `FFmpeg_*.c` wrappers in
`src/vendor_shims/ffmpeg/` make each selected upstream C source its own Arduino
translation unit. Quoted includes that relied on FFmpeg's top-level include
directory (`libavutil/...`, `libavcodec/...`, `compat/...`) are rewritten
relative to the including file by `tools/localize_vendor_includes.py`; this
include-path-only patch changes no declarations, definitions, or behavior. `config.h`
and `config_components.h` record that constrained configuration.

The source is distributed under the LGPL version 2.1 or later. See
`COPYING.LGPLv2.1` and `LICENSE.md` for the full upstream notices.
