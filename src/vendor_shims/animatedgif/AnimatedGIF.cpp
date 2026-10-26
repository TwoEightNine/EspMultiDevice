// AnimatedGIF 2.2.3 source wrapper; compiled once as its own translation unit
// because Arduino's sketch builder does not compile sources outside src/.
// LZW decode dominates animated GIF playback, so build the pinned sources at
// -O2 instead of the Arduino default -Os; the vendored files stay unmodified.
#if defined(__GNUC__)
#pragma GCC optimize("O2")
#endif
#include "../../../third_party/AnimatedGIF/AnimatedGIF.cpp"
