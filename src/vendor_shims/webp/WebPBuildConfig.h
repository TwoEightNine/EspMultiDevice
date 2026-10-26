#pragma once

// VP8/VP8L decode and their DSP stages dominate WebP cost for animations,
// stills, and thumbnails. Build the pinned libwebp sources at -O2 instead of
// the Arduino default -Os; the vendored files stay unmodified.
#if defined(__GNUC__)
#pragma GCC optimize("O2")
#endif
