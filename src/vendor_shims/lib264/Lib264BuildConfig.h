#pragma once

// H.264 decode is the video worker's dominant cost. Build the pinned lib264
// sources at -O2 instead of the Arduino default -Os; the vendored files stay
// unmodified.
#if defined(__GNUC__)
#pragma GCC optimize("O2")
#endif
