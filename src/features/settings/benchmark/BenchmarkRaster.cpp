// The span loops dominate the benchmark; the rest of the sketch keeps the
// Arduino default -Os.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "BenchmarkRaster.h"

#include <math.h>
#include <string.h>

namespace BenchRaster {
namespace {

// Row and column coverage follow pixel centres, so shared edges are drawn once.
inline int16_t coverStart(float edge) {
  return (int16_t)ceilf(edge - 0.5f);
}

// Clamped colour attributes: interpolation stays inside the vertex range, so
// keeping vertices away from 0 and 255 avoids fixed-point wrap at edges.
inline float clampColor(float value) {
  return value < 1.0f ? 1.0f : (value > 254.0f ? 254.0f : value);
}

inline int32_t toFixed(float value) {
  return (int32_t)(value * 65536.0f);
}

template <bool Flat>
void drawTri(const Band &band, const Tri &tri, int16_t yStart, int16_t yEnd) {
  const int16_t bandEnd = band.y0 + band.rows;
  const int16_t first = yStart > band.y0 ? yStart : band.y0;
  const int16_t last = yEnd < bandEnd ? yEnd : bandEnd;
  for (int16_t y = first; y < last; ++y) {
    const float py = y + 0.5f;
    const float longX = tri.x0 + (py - tri.y0) * tri.invLong;
    const float shortX = py < tri.y1 ? tri.x0 + (py - tri.y0) * tri.invTop
                                     : tri.x1 + (py - tri.y1) * tri.invBottom;
    float left = longX;
    float right = shortX;
    if (left > right) {
      left = shortX;
      right = longX;
    }
    int16_t xs = coverStart(left);
    int16_t xe = coverStart(right);
    if (xs < 0) xs = 0;
    if (xe > Width) xe = Width;
    if (xs >= xe) continue;

    const float dx = xs + 0.5f - tri.x0;
    const float dy = py - tri.y0;
    const int32_t row = (int32_t)(y - band.y0) * Width;
    uint16_t *pixel = band.pixels + row + xs;
    uint16_t *depth = band.depth + row + xs;
    int32_t z = toFixed(tri.z0 + tri.dzdx * dx + tri.dzdy * dy);
    const int32_t dz = toFixed(tri.dzdx);
    int16_t count = xe - xs;

    if (Flat) {
      const uint16_t color = tri.flatColor;
      while (count-- > 0) {
        const uint16_t value = (uint16_t)(z >> 16);
        if (value > *depth) {
          *depth = value;
          *pixel = color;
        }
        ++pixel;
        ++depth;
        z += dz;
      }
    } else {
      int32_t r = toFixed(tri.r0 + tri.drdx * dx + tri.drdy * dy);
      int32_t g = toFixed(tri.g0 + tri.dgdx * dx + tri.dgdy * dy);
      int32_t b = toFixed(tri.b0 + tri.dbdx * dx + tri.dbdy * dy);
      const int32_t dr = toFixed(tri.drdx);
      const int32_t dg = toFixed(tri.dgdx);
      const int32_t db = toFixed(tri.dbdx);
      while (count-- > 0) {
        const uint16_t value = (uint16_t)(z >> 16);
        if (value > *depth) {
          *depth = value;
          *pixel = (uint16_t)((((uint32_t)r >> 8) & 0xF800) | (((uint32_t)g >> 13) & 0x07E0) |
                              (((uint32_t)b >> 19) & 0x001F));
        }
        ++pixel;
        ++depth;
        z += dz;
        r += dr;
        g += dg;
        b += db;
      }
    }
  }
}

}  // namespace

Vec3 normalize(const Vec3 &v) {
  const float length = sqrtf(dot(v, v));
  if (length <= 1e-12f) return Vec3{0.0f, 0.0f, 1.0f};
  return v * (1.0f / length);
}

Mat3 rotation(float pitch, float yaw, float roll) {
  const float cp = cosf(pitch), sp = sinf(pitch);
  const float cy = cosf(yaw), sy = sinf(yaw);
  const float cr = cosf(roll), sr = sinf(roll);
  const Mat3 x = {{1, 0, 0, 0, cp, -sp, 0, sp, cp}};
  const Mat3 y = {{cy, 0, sy, 0, 1, 0, -sy, 0, cy}};
  const Mat3 z = {{cr, -sr, 0, sr, cr, 0, 0, 0, 1}};
  return multiply(z, multiply(y, x));
}

Mat3 multiply(const Mat3 &a, const Mat3 &b) {
  Mat3 out;
  for (uint8_t row = 0; row < 3; ++row) {
    for (uint8_t column = 0; column < 3; ++column) {
      out.m[row * 3 + column] = a.m[row * 3] * b.m[column] + a.m[row * 3 + 1] * b.m[3 + column] +
                                a.m[row * 3 + 2] * b.m[6 + column];
    }
  }
  return out;
}

bool setupTri(Tri &tri, int16_t &yStart, int16_t &yEnd,
              const ProjectedVertex &a, const ProjectedVertex &b, const ProjectedVertex &c,
              int16_t canvasHeight, bool flat, uint16_t flatColor) {
  const ProjectedVertex *v0 = &a;
  const ProjectedVertex *v1 = &b;
  const ProjectedVertex *v2 = &c;
  if (v1->y < v0->y) { const ProjectedVertex *t = v0; v0 = v1; v1 = t; }
  if (v2->y < v1->y) { const ProjectedVertex *t = v1; v1 = v2; v2 = t; }
  if (v1->y < v0->y) { const ProjectedVertex *t = v0; v0 = v1; v1 = t; }

  const float minX = fminf(v0->x, fminf(v1->x, v2->x));
  const float maxX = fmaxf(v0->x, fmaxf(v1->x, v2->x));
  if (maxX < 0.0f || minX > Width) return false;

  int16_t start = coverStart(v0->y);
  int16_t end = coverStart(v2->y);
  if (start < 0) start = 0;
  if (end > canvasHeight) end = canvasHeight;
  if (start >= end) return false;

  const float e1x = v1->x - v0->x, e1y = v1->y - v0->y;
  const float e2x = v2->x - v0->x, e2y = v2->y - v0->y;
  const float det = e1x * e2y - e2x * e1y;
  if (fabsf(det) < 1e-4f) return false;
  const float inv = 1.0f / det;

  tri.x0 = v0->x; tri.y0 = v0->y;
  tri.x1 = v1->x; tri.y1 = v1->y;
  tri.x2 = v2->x; tri.y2 = v2->y;
  tri.invLong = e2x / e2y;
  tri.invTop = e1y > 0.0f ? e1x / e1y : 0.0f;
  tri.invBottom = (v2->y - v1->y) > 0.0f ? (v2->x - v1->x) / (v2->y - v1->y) : 0.0f;

  const float dz1 = v1->depth - v0->depth, dz2 = v2->depth - v0->depth;
  tri.z0 = v0->depth;
  tri.dzdx = (dz1 * e2y - dz2 * e1y) * inv;
  tri.dzdy = (dz2 * e1x - dz1 * e2x) * inv;

  tri.flat = flat;
  tri.flatColor = flatColor;
  if (!flat) {
    const float r0 = clampColor(v0->r), r1 = clampColor(v1->r), r2 = clampColor(v2->r);
    const float g0 = clampColor(v0->g), g1 = clampColor(v1->g), g2 = clampColor(v2->g);
    const float b0 = clampColor(v0->b), b1 = clampColor(v1->b), b2 = clampColor(v2->b);
    tri.r0 = r0;
    tri.drdx = ((r1 - r0) * e2y - (r2 - r0) * e1y) * inv;
    tri.drdy = ((r2 - r0) * e1x - (r1 - r0) * e2x) * inv;
    tri.g0 = g0;
    tri.dgdx = ((g1 - g0) * e2y - (g2 - g0) * e1y) * inv;
    tri.dgdy = ((g2 - g0) * e1x - (g1 - g0) * e2x) * inv;
    tri.b0 = b0;
    tri.dbdx = ((b1 - b0) * e2y - (b2 - b0) * e1y) * inv;
    tri.dbdy = ((b2 - b0) * e1x - (b1 - b0) * e2x) * inv;
  }
  yStart = start;
  yEnd = end;
  return true;
}

void clearDepth(const Band &band) {
  memset(band.depth, 0, (size_t)band.rows * Width * sizeof(uint16_t));
}

void drawTris(const Band &band, const Tri *tris, const int16_t *yRanges, uint16_t count) {
  const int16_t bandEnd = band.y0 + band.rows;
  for (uint16_t index = 0; index < count; ++index) {
    const int16_t yStart = yRanges[index * 2];
    const int16_t yEnd = yRanges[index * 2 + 1];
    if (yEnd <= band.y0 || yStart >= bandEnd) continue;
    const Tri &tri = tris[index];
    if (tri.flat) {
      drawTri<true>(band, tri, yStart, yEnd);
    } else {
      drawTri<false>(band, tri, yStart, yEnd);
    }
  }
}

void hsv(float hue, float saturation, float value, float &r, float &g, float &b) {
  hue -= floorf(hue);
  const float h = hue * 6.0f;
  const int sector = (int)h;
  const float f = h - sector;
  const float p = value * (1.0f - saturation);
  const float q = value * (1.0f - saturation * f);
  const float t = value * (1.0f - saturation * (1.0f - f));
  float rr, gg, bb;
  switch (sector) {
    case 0: rr = value; gg = t; bb = p; break;
    case 1: rr = q; gg = value; bb = p; break;
    case 2: rr = p; gg = value; bb = t; break;
    case 3: rr = p; gg = q; bb = value; break;
    case 4: rr = t; gg = p; bb = value; break;
    default: rr = value; gg = p; bb = q; break;
  }
  r = rr * 255.0f;
  g = gg * 255.0f;
  b = bb * 255.0f;
}

uint16_t hsv565(float hue, float saturation, float value) {
  float r, g, b;
  hsv(hue, saturation, value, r, g, b);
  return rgb565((int)r, (int)g, (int)b);
}

uint16_t lerp565(uint16_t a, uint16_t b, uint8_t t) {
  const int ar = a >> 11, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  const int br = b >> 11, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  const int r = ar + (((br - ar) * t) >> 8);
  const int g = ag + (((bg - ag) * t) >> 8);
  const int bl = ab + (((bb - ab) * t) >> 8);
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

}  // namespace BenchRaster
