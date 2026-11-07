#pragma once

#include <Arduino.h>

// Small software rasterizer used only by the Settings benchmark. Frames are
// rendered as horizontal bands so each worker keeps its depth buffer in fast
// internal RAM while writing colour directly into a shared PSRAM frame slot.
namespace BenchRaster {

constexpr int16_t Width = 320;
constexpr int16_t BandHeight = 32;
// Depth is stored as a 16-bit reciprocal: DepthScale * NearZ / z. Larger
// values are nearer; 0 is the cleared far plane.
constexpr float NearZ = 0.5f;
constexpr float DepthScale = 30000.0f;

struct Vec3 {
  float x;
  float y;
  float z;
};

inline Vec3 vec3(float x, float y, float z) { return Vec3{x, y, z}; }
inline Vec3 operator+(const Vec3 &a, const Vec3 &b) { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3 &a, const Vec3 &b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3 &a, float s) { return Vec3{a.x * s, a.y * s, a.z * s}; }
inline float dot(const Vec3 &a, const Vec3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3 &a, const Vec3 &b) {
  return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3 normalize(const Vec3 &v);

// Row-major 3x3 rotation.
struct Mat3 {
  float m[9];
  Vec3 apply(const Vec3 &v) const {
    return Vec3{m[0] * v.x + m[1] * v.y + m[2] * v.z,
                m[3] * v.x + m[4] * v.y + m[5] * v.z,
                m[6] * v.x + m[7] * v.y + m[8] * v.z};
  }
};
// Rotation about X (pitch), then Y (yaw), then Z (roll).
Mat3 rotation(float pitch, float yaw, float roll);
Mat3 multiply(const Mat3 &a, const Mat3 &b);

// One band of the canvas. `pixels` addresses canvas row y0 inside the frame
// slot; `depth` is band-local and Width * BandHeight entries long.
struct Band {
  uint16_t *pixels;
  uint16_t *depth;
  int16_t y0;
  int16_t rows;
};

struct ProjectedVertex {
  float x;
  float y;
  float depth;
  float r;
  float g;
  float b;
};

// A set-up triangle. Vertices are sorted by y; attributes are planes anchored
// at the first vertex so a band can start any row without walking edges.
struct Tri {
  float x0, y0, x1, y1, x2, y2;
  float invLong, invTop, invBottom;
  float z0, dzdx, dzdy;
  float r0, drdx, drdy;
  float g0, dgdx, dgdy;
  float b0, dbdx, dbdy;
  uint16_t flatColor;
  bool flat;
};

// Prepares `tri` and its [yStart, yEnd) row range. Returns false when the
// triangle is degenerate or outside a canvas of `canvasHeight` rows.
bool setupTri(Tri &tri, int16_t &yStart, int16_t &yEnd,
              const ProjectedVertex &a, const ProjectedVertex &b, const ProjectedVertex &c,
              int16_t canvasHeight, bool flat, uint16_t flatColor);

void clearDepth(const Band &band);
// Draws every triangle whose [yRanges[2i], yRanges[2i+1]) overlaps the band.
void drawTris(const Band &band, const Tri *tris, const int16_t *yRanges, uint16_t count);

inline uint16_t rgb565(int r, int g, int b) {
  r = r < 0 ? 0 : (r > 255 ? 255 : r);
  g = g < 0 ? 0 : (g > 255 ? 255 : g);
  b = b < 0 ? 0 : (b > 255 ? 255 : b);
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Saturating per-channel RGB565 addition.
inline uint16_t addSaturate(uint16_t a, uint16_t b) {
  int r = (a >> 11) + (b >> 11);
  int g = ((a >> 5) & 0x3F) + ((b >> 5) & 0x3F);
  int bl = (a & 0x1F) + (b & 0x1F);
  if (r > 31) r = 31;
  if (g > 63) g = 63;
  if (bl > 31) bl = 31;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

// Hue in turns (wraps), saturation and value in 0..1; outputs 0..255.
void hsv(float hue, float saturation, float value, float &r, float &g, float &b);
uint16_t hsv565(float hue, float saturation, float value);
uint16_t lerp565(uint16_t a, uint16_t b, uint8_t t);

inline uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}
inline float hashUnit(uint32_t x) { return (hash32(x) >> 8) * (1.0f / 16777216.0f); }

}  // namespace BenchRaster
