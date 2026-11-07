// Per-pixel background effects and vertex work dominate the benchmark; the
// rest of the sketch keeps the Arduino default -Os.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "BenchmarkScenes.h"

#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

using namespace BenchRaster;

namespace {

constexpr float TwoPi = 6.28318531f;
constexpr float Focal = 380.0f;
constexpr int16_t MaxCanvasHeight = 384;

// Torus knot (p=2, q=3).
constexpr uint16_t KnotTubular = 160;
constexpr uint16_t KnotRadial = 12;
constexpr uint16_t KnotVertexCount = KnotTubular * KnotRadial;
constexpr uint16_t KnotTriCount = KnotVertexCount * 2;

// Crystal: icosphere subdivided twice, one large and six orbiting copies.
constexpr uint16_t CrystalMaxVertices = 162;
constexpr uint16_t CrystalMaxTris = 320;
constexpr uint8_t CrystalInstances = 7;

constexpr uint16_t StarCount = 1400;
constexpr float StarDepth = 40.0f;

constexpr uint16_t MaxVertices = KnotVertexCount;
constexpr uint16_t MaxTris = KnotTriCount;

constexpr uint16_t GlowEntries = 1024;
constexpr float CrystalCameraHeight = 2.6f;
constexpr float FloorFogDistance = 34.0f;

template <typename T>
T *allocatePsram(size_t count) {
  return static_cast<T *>(heap_caps_malloc(count * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

template <typename T>
T *allocateFast(size_t count) {
  void *memory = heap_caps_malloc(count * sizeof(T), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!memory) memory = heap_caps_malloc(count * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return static_cast<T *>(memory);
}

template <typename T>
void release(T *&pointer) {
  if (pointer) heap_caps_free(pointer);
  pointer = nullptr;
}

inline float clamp01(float value) {
  return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

inline float easeOutCubic(float t) {
  t = 1.0f - clamp01(t);
  return 1.0f - t * t * t;
}

inline uint16_t scale565(uint16_t color, uint16_t amount) {
  const uint16_t r = ((color >> 11) * amount) >> 8;
  const uint16_t g = (((color >> 5) & 0x3F) * amount) >> 8;
  const uint16_t b = ((color & 0x1F) * amount) >> 8;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

inline bool project(const Vec3 &p, float cx, float cy, ProjectedVertex &out) {
  if (p.z < NearZ) {
    out.depth = -1.0f;
    return false;
  }
  const float iz = 1.0f / p.z;
  out.x = cx + Focal * p.x * iz;
  out.y = cy - Focal * p.y * iz;
  out.depth = DepthScale * NearZ * iz;
  return true;
}

Vec3 knotCurve(float u) {
  const float radius = 1.6f;
  const float quOverP = 1.5f * u;
  const float cs = cosf(quOverP);
  return vec3(radius * (2.0f + cs) * 0.5f * cosf(u), radius * (2.0f + cs) * 0.5f * sinf(u),
              radius * sinf(quOverP) * 0.5f);
}

}  // namespace

BenchmarkScenes::~BenchmarkScenes() {
  end();
}

bool BenchmarkScenes::begin() {
  if (_tris) return true;
  _tris = allocatePsram<Tri>(MaxTris);
  _triY = allocatePsram<int16_t>(MaxTris * 2);
  _camera = allocatePsram<Vec3>(MaxVertices);
  _projected = allocatePsram<ProjectedVertex>(MaxVertices);
  _segments = allocatePsram<Segment>(StarCount);
  _falloff = allocateFast<uint8_t>(GlowEntries);
  _glow = allocateFast<uint16_t>(GlowEntries);
  _rowColors = allocatePsram<uint16_t>(MaxCanvasHeight);
  _rowLineColors = allocatePsram<uint16_t>(MaxCanvasHeight);
  _rowLineScale = allocatePsram<int32_t>(MaxCanvasHeight);
  _rowLineZ = allocatePsram<uint8_t>(MaxCanvasHeight);
  _rowWorldX0 = allocatePsram<int32_t>(MaxCanvasHeight);
  _rowWorldStep = allocatePsram<int32_t>(MaxCanvasHeight);
  _palette = allocateFast<uint16_t>(16 * 256);
  if (!_tris || !_triY || !_camera || !_projected || !_segments || !_falloff || !_glow ||
      !_rowColors || !_rowLineColors || !_rowLineScale || !_rowLineZ || !_rowWorldX0 ||
      !_rowWorldStep || !_palette) {
    end();
    return false;
  }
  for (uint16_t i = 0; i < GlowEntries; ++i) {
    // Entry i holds squared distance i * 64, so distance is 8 * sqrt(i).
    const float distance = 8.0f * sqrtf((float)i);
    const float f = 1.0f - clamp01(distance / 230.0f);
    _falloff[i] = (uint8_t)(f * f * 255.0f);
  }
  for (uint16_t i = 0; i < 256; ++i) {
    _sine[i] = (uint8_t)(42.5f + 42.0f * sinf(i * TwoPi / 256.0f));
  }
  for (int16_t x = 0; x < Width; ++x) {
    const int32_t dx = x - Width / 2;
    _dx2[x] = dx * dx;
  }
  return true;
}

void BenchmarkScenes::end() {
  release(_tris);
  release(_triY);
  release(_camera);
  release(_projected);
  release(_knotPositions);
  release(_knotNormals);
  release(_knotHue);
  release(_knotIndices);
  _knotReady = false;
  release(_crystalPositions);
  release(_crystalIndices);
  _crystalVertexCount = 0;
  _crystalTriCount = 0;
  release(_segments);
  release(_falloff);
  release(_glow);
  release(_rowColors);
  release(_rowLineColors);
  release(_rowLineScale);
  release(_rowLineZ);
  release(_rowWorldX0);
  release(_rowWorldStep);
  release(_tunnel);
  _tunnelHeight = 0;
  release(_palette);
  _triCount = 0;
  _segmentCount = 0;
}

bool BenchmarkScenes::prepare(BenchScene scene, int16_t height) {
  if (!_tris || height <= 0 || height > MaxCanvasHeight) return false;
  _height = height;
  _triCount = 0;
  _segmentCount = 0;
  switch (scene) {
    case BenchScene::Knot: return buildKnot();
    case BenchScene::Crystal:
      buildFloorRows(height);
      return buildCrystal();
    case BenchScene::Tunnel: return buildTunnel(height);
    case BenchScene::Warp:
    case BenchScene::Score: return true;
    default: return false;
  }
}

bool BenchmarkScenes::buildKnot() {
  if (_knotReady) return true;
  if (!_knotPositions) _knotPositions = allocatePsram<Vec3>(KnotVertexCount);
  if (!_knotNormals) _knotNormals = allocatePsram<Vec3>(KnotVertexCount);
  if (!_knotHue) _knotHue = allocatePsram<float>(KnotVertexCount);
  if (!_knotIndices) _knotIndices = allocatePsram<uint16_t>(KnotTriCount * 3);
  if (!_knotPositions || !_knotNormals || !_knotHue || !_knotIndices) return false;

  const float tube = 0.5f;
  for (uint16_t i = 0; i < KnotTubular; ++i) {
    const float u = (float)i / KnotTubular * 2.0f * TwoPi;
    const Vec3 p1 = knotCurve(u);
    const Vec3 p2 = knotCurve(u + 0.01f);
    const Vec3 tangent = p2 - p1;
    Vec3 normal = p2 + p1;
    const Vec3 binormal = normalize(cross(tangent, normal));
    normal = normalize(cross(binormal, tangent));
    for (uint16_t j = 0; j < KnotRadial; ++j) {
      const float v = (float)j / KnotRadial * TwoPi;
      const float cx = -tube * cosf(v);
      const float cy = tube * sinf(v);
      const Vec3 position = p1 + normal * cx + binormal * cy;
      const uint16_t index = i * KnotRadial + j;
      _knotPositions[index] = position;
      _knotNormals[index] = normalize(position - p1);
      _knotHue[index] = (float)i / KnotTubular;
    }
  }
  uint32_t out = 0;
  for (uint16_t i = 0; i < KnotTubular; ++i) {
    const uint16_t next = (i + 1) % KnotTubular;
    for (uint16_t j = 0; j < KnotRadial; ++j) {
      const uint16_t j1 = (j + 1) % KnotRadial;
      const uint16_t a = i * KnotRadial + j;
      const uint16_t b = next * KnotRadial + j;
      const uint16_t c = next * KnotRadial + j1;
      const uint16_t d = i * KnotRadial + j1;
      const uint16_t quads[2][3] = {{a, b, d}, {b, c, d}};
      for (uint8_t q = 0; q < 2; ++q) {
        uint16_t i0 = quads[q][0], i1 = quads[q][1], i2 = quads[q][2];
        const Vec3 face = cross(_knotPositions[i1] - _knotPositions[i0], _knotPositions[i2] - _knotPositions[i0]);
        const Vec3 outward = _knotNormals[i0] + _knotNormals[i1] + _knotNormals[i2];
        if (dot(face, outward) < 0.0f) {
          const uint16_t swap = i1;
          i1 = i2;
          i2 = swap;
        }
        _knotIndices[out++] = i0;
        _knotIndices[out++] = i1;
        _knotIndices[out++] = i2;
      }
    }
  }
  _knotReady = true;
  return true;
}

bool BenchmarkScenes::buildCrystal() {
  if (_crystalTriCount) return true;
  if (!_crystalPositions) _crystalPositions = allocatePsram<Vec3>(CrystalMaxVertices);
  if (!_crystalIndices) _crystalIndices = allocatePsram<uint16_t>(CrystalMaxTris * 3);
  uint16_t *scratch = allocatePsram<uint16_t>(CrystalMaxTris * 3);
  if (!_crystalPositions || !_crystalIndices || !scratch) {
    release(scratch);
    return false;
  }

  const float t = 1.6180340f;
  const Vec3 base[12] = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                         {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
  const uint8_t faces[20][3] = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11},
                                {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                                {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9},
                                {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
  uint16_t vertexCount = 12;
  for (uint8_t i = 0; i < 12; ++i) _crystalPositions[i] = normalize(base[i]);
  uint16_t triCount = 20;
  for (uint8_t i = 0; i < 20; ++i) {
    for (uint8_t k = 0; k < 3; ++k) _crystalIndices[i * 3 + k] = faces[i][k];
  }

  auto midpoint = [&](uint16_t a, uint16_t b) -> uint16_t {
    const Vec3 m = normalize((_crystalPositions[a] + _crystalPositions[b]) * 0.5f);
    for (uint16_t i = 0; i < vertexCount; ++i) {
      const Vec3 d = _crystalPositions[i] - m;
      if (dot(d, d) < 1e-6f) return i;
    }
    _crystalPositions[vertexCount] = m;
    return vertexCount++;
  };

  for (uint8_t level = 0; level < 2; ++level) {
    uint16_t out = 0;
    for (uint16_t f = 0; f < triCount; ++f) {
      const uint16_t a = _crystalIndices[f * 3], b = _crystalIndices[f * 3 + 1], c = _crystalIndices[f * 3 + 2];
      const uint16_t ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
      const uint16_t next[4][3] = {{a, ab, ca}, {b, bc, ab}, {c, ca, bc}, {ab, bc, ca}};
      for (uint8_t n = 0; n < 4; ++n) {
        for (uint8_t k = 0; k < 3; ++k) scratch[out++] = next[n][k];
      }
    }
    triCount *= 4;
    memcpy(_crystalIndices, scratch, (size_t)triCount * 3 * sizeof(uint16_t));
  }
  release(scratch);

  // Deterministic radial displacement turns the sphere into a faceted rock.
  for (uint16_t i = 0; i < vertexCount; ++i) {
    const float radius = 0.72f + 0.56f * hashUnit(i * 7919u + 17u);
    _crystalPositions[i] = _crystalPositions[i] * radius;
  }
  for (uint16_t f = 0; f < triCount; ++f) {
    uint16_t *tri = _crystalIndices + f * 3;
    const Vec3 a = _crystalPositions[tri[0]], b = _crystalPositions[tri[1]], c = _crystalPositions[tri[2]];
    if (dot(cross(b - a, c - a), a + b + c) < 0.0f) {
      const uint16_t swap = tri[1];
      tri[1] = tri[2];
      tri[2] = swap;
    }
  }
  _crystalVertexCount = vertexCount;
  _crystalTriCount = triCount;
  return true;
}

bool BenchmarkScenes::buildTunnel(int16_t height) {
  if (_tunnel && _tunnelHeight == height) return true;
  release(_tunnel);
  _tunnel = allocatePsram<uint32_t>((size_t)Width * height);
  if (!_tunnel) return false;
  const float cx = Width * 0.5f;
  const float cy = height * 0.5f;
  const float maxDistance = sqrtf(cx * cx + cy * cy);
  for (int16_t y = 0; y < height; ++y) {
    uint32_t *row = _tunnel + (size_t)y * Width;
    const float dy = y + 0.5f - cy;
    for (int16_t x = 0; x < Width; ++x) {
      const float dx = x + 0.5f - cx;
      const float distance = sqrtf(dx * dx + dy * dy);
      const uint8_t angle = (uint8_t)(int32_t)(atan2f(dy, dx) * (512.0f / TwoPi));
      const uint8_t depth = (uint8_t)(int32_t)(6000.0f / (distance + 1.0f));
      const float s = clamp01(distance / (maxDistance * 0.8f));
      const uint8_t shade = (uint8_t)(s * 15.99f);
      row[x] = angle | ((uint32_t)depth << 8) | ((uint32_t)shade << 16);
    }
  }
  _tunnelHeight = height;
  return true;
}

void BenchmarkScenes::buildFloorRows(int16_t height) {
  _horizon = (int16_t)(height * 0.40f);
  _sunRadius = 100.0f;
  _sunCenterY = _horizon - 50.0f;
  const uint16_t skyTop = rgb565(6, 4, 28);
  const uint16_t skyHorizon = rgb565(150, 40, 120);
  const uint16_t floorNear = rgb565(22, 0, 34);
  const uint16_t floorFar = rgb565(110, 30, 110);
  const uint16_t neon = rgb565(255, 50, 210);
  for (int16_t y = 0; y < height; ++y) {
    if (y < _horizon) {
      const float s = (float)y / _horizon;
      _rowColors[y] = lerp565(skyTop, skyHorizon, (uint8_t)(s * s * 255.0f));
      _rowLineColors[y] = _rowColors[y];
      _rowLineScale[y] = 0;
      _rowWorldX0[y] = 0;
      _rowWorldStep[y] = 0;
      continue;
    }
    const float below = y + 0.5f - _horizon;
    const float distance = CrystalCameraHeight * Focal / below;
    const float fog = clamp01(1.0f - distance / FloorFogDistance);
    _rowColors[y] = lerp565(floorFar, floorNear, (uint8_t)(fog * 255.0f));
    _rowLineColors[y] = lerp565(_rowColors[y], neon, (uint8_t)(fog * fog * 255.0f));
    const float pixelWorld = distance / Focal;
    float scale = 255.0f / (pixelWorld * 1.3f);
    if (scale > 60000.0f) scale = 60000.0f;
    _rowLineScale[y] = (int32_t)scale;
    _rowWorldX0[y] = (int32_t)((0.5f - Width * 0.5f) * pixelWorld * 65536.0f);
    _rowWorldStep[y] = (int32_t)(pixelWorld * 65536.0f);
  }
}

void BenchmarkScenes::buildGlow(uint16_t inner, uint16_t outer) {
  for (uint16_t i = 0; i < GlowEntries; ++i) _glow[i] = lerp565(outer, inner, _falloff[i]);
}

bool BenchmarkScenes::emitTri(const ProjectedVertex &a, const ProjectedVertex &b, const ProjectedVertex &c,
                              bool flat, uint16_t color) {
  if (_triCount >= MaxTris) return false;
  int16_t yStart, yEnd;
  if (!setupTri(_tris[_triCount], yStart, yEnd, a, b, c, _height, flat, color)) return false;
  _triY[_triCount * 2] = yStart;
  _triY[_triCount * 2 + 1] = yEnd;
  ++_triCount;
  return true;
}

void BenchmarkScenes::setupFrame(BenchScene scene, const BenchFrameParams &params) {
  _height = params.height;
  _triCount = 0;
  _segmentCount = 0;
  switch (scene) {
    case BenchScene::Warp: setupWarp(params); break;
    case BenchScene::Crystal: setupCrystal(params); break;
    case BenchScene::Knot: setupKnot(params); break;
    case BenchScene::Tunnel: setupTunnel(params); break;
    case BenchScene::Score: setupScore(params); break;
    default: break;
  }
}

void BenchmarkScenes::setupWarp(const BenchFrameParams &params) {
  const float t = params.t;
  const float travel = 4.0f * t + 2.2f * t * t;
  const float speed = 4.0f + 4.4f * t;
  const float streak = 0.03f + speed * 0.045f;
  const float roll = t * 0.25f;
  const float cr = cosf(roll), sr = sinf(roll);
  const float cx = Width * 0.5f;
  const float cy = params.height * 0.5f;
  const float energy = clamp01(t / 8.0f);
  buildGlow(rgb565((int)(30 + 50 * energy), (int)(30 + 40 * energy), (int)(96 + 120 * energy)), 0);

  const uint16_t tints[3] = {rgb565(210, 225, 255), rgb565(90, 230, 255), rgb565(255, 120, 220)};
  uint16_t count = 0;
  for (uint16_t i = 0; i < StarCount; ++i) {
    const uint32_t seed = i * 2654435761u;
    const float angle = hashUnit(seed + 1) * TwoPi;
    const float radius = 0.35f + 5.0f * sqrtf(hashUnit(seed + 2));
    const float phase = hashUnit(seed + 3) * StarDepth;
    float z = StarDepth - fmodf(travel + phase, StarDepth);
    if (z < 0.6f) continue;
    const float lx = cosf(angle) * radius, ly = sinf(angle) * radius;
    const float x = lx * cr - ly * sr;
    const float y = lx * sr + ly * cr;
    ProjectedVertex head, tail;
    project(vec3(x, y, z), cx, cy, head);
    project(vec3(x, y, z + streak), cx, cy, tail);
    if ((head.x < -2 && tail.x < -2) || (head.x > Width + 2 && tail.x > Width + 2) ||
        (head.y < -2 && tail.y < -2) || (head.y > params.height + 2 && tail.y > params.height + 2)) {
      continue;
    }
    if (fabsf(head.x) > 4000.0f || fabsf(head.y) > 4000.0f) continue;
    const float nearness = clamp01((1.0f - z / StarDepth) * 1.3f);
    const float brightness = nearness * nearness;
    if (brightness < 0.04f) continue;
    const uint8_t tint = hashUnit(seed + 4) < 0.6f ? 0 : (hashUnit(seed + 5) < 0.6f ? 1 : 2);
    Segment &segment = _segments[count++];
    segment.x0 = (int16_t)tail.x;
    segment.y0 = (int16_t)tail.y;
    segment.x1 = (int16_t)head.x;
    segment.y1 = (int16_t)head.y;
    segment.yMin = min(segment.y0, segment.y1) - 1;
    segment.yMax = max(segment.y0, segment.y1) + 2;
    segment.color = scale565(tints[tint], (uint16_t)(256 * brightness));
    segment.headSize = z < 7.0f ? 2 : 1;
  }
  _segmentCount = count;
}

void BenchmarkScenes::setupCrystal(const BenchFrameParams &params) {
  const float t = params.t;
  const float cx = Width * 0.5f;
  const float cy = (float)_horizon;
  _sunPhase = t * 0.35f;
  _floorSway = (int32_t)(sinf(t * 0.4f) * 0.8f * 65536.0f);

  // Horizontal grid lines scroll toward the viewer.
  const float scroll = t * 3.2f;
  for (int16_t y = _horizon; y < params.height; ++y) {
    const float below = y + 0.5f - _horizon;
    const float distance = CrystalCameraHeight * Focal / below;
    const float worldZ = distance + scroll;
    float fraction = worldZ - floorf(worldZ);
    if (fraction > 0.5f) fraction = 1.0f - fraction;
    const float rowWorld = distance * distance / (CrystalCameraHeight * Focal);
    const float intensity = clamp01(1.0f - fraction / (rowWorld * 1.3f + 0.02f));
    _rowLineZ[y] = (uint8_t)(intensity * 255.0f);
  }

  const Vec3 key = normalize(vec3(0.6f, 0.7f, -0.5f));
  const Vec3 fill = normalize(vec3(-0.8f, -0.25f, -0.4f));
  const Vec3 half = normalize(key + vec3(0.0f, 0.0f, -1.0f));

  for (uint8_t instance = 0; instance < CrystalInstances; ++instance) {
    Vec3 offset;
    Vec3 scale;
    Mat3 spin;
    float hue;
    if (instance == 0) {
      offset = vec3(0.0f, 0.2f + 0.12f * sinf(t * 1.1f), 7.5f);
      scale = vec3(1.2f, 1.7f, 1.2f);
      spin = rotation(0.25f * sinf(t * 0.7f), t * 0.55f, 0.1f * sinf(t * 0.5f));
      hue = 0.52f;
    } else {
      const float orbit = t * 0.6f + (instance - 1) * (TwoPi / 6.0f);
      offset = vec3(cosf(orbit) * 2.6f, 0.2f + 0.55f * sinf(t * 1.3f + instance), 7.5f + sinf(orbit) * 1.9f);
      const float s = 0.34f + 0.05f * (instance & 1);
      scale = vec3(s, s * 1.4f, s);
      spin = rotation(t * (0.9f + 0.1f * instance), t * 1.3f + instance, 0.0f);
      hue = 0.74f + 0.04f * instance;
    }
    float baseR, baseG, baseB;
    hsv(hue, 0.55f, 1.0f, baseR, baseG, baseB);

    const uint16_t first = instance * _crystalVertexCount;
    for (uint16_t i = 0; i < _crystalVertexCount; ++i) {
      const Vec3 local = _crystalPositions[i];
      const Vec3 scaled = vec3(local.x * scale.x, local.y * scale.y, local.z * scale.z);
      _camera[first + i] = spin.apply(scaled) + offset;
      project(_camera[first + i], cx, cy, _projected[first + i]);
    }
    for (uint16_t f = 0; f < _crystalTriCount; ++f) {
      const uint16_t *tri = _crystalIndices + f * 3;
      const uint16_t i0 = first + tri[0], i1 = first + tri[1], i2 = first + tri[2];
      if (_projected[i0].depth < 0 || _projected[i1].depth < 0 || _projected[i2].depth < 0) continue;
      const Vec3 a = _camera[i0];
      const Vec3 normal = normalize(cross(_camera[i1] - a, _camera[i2] - a));
      if (dot(normal, a) >= 0.0f) continue;
      const float diffuse = fmaxf(0.0f, dot(normal, key));
      const float back = fmaxf(0.0f, dot(normal, fill));
      float spec = fmaxf(0.0f, dot(normal, half));
      spec *= spec;
      spec *= spec;
      spec *= spec;
      spec *= spec;
      const float facet = 0.85f + 0.15f * hashUnit(f * 31u + instance);
      const float r = baseR * (0.10f + 0.80f * diffuse) * facet + 255.0f * back * 0.55f + 255.0f * spec;
      const float g = baseG * (0.10f + 0.80f * diffuse) * facet + 60.0f * back * 0.55f + 245.0f * spec;
      const float b = baseB * (0.10f + 0.80f * diffuse) * facet + 210.0f * back * 0.55f + 235.0f * spec;
      emitTri(_projected[i0], _projected[i1], _projected[i2], true, rgb565((int)r, (int)g, (int)b));
    }
  }
}

void BenchmarkScenes::setupKnot(const BenchFrameParams &params) {
  const float t = params.t;
  const float cx = Width * 0.5f;
  const float cy = params.height * 0.5f;
  const float flash = clamp01(params.flash);
  buildGlow(rgb565((int)(34 + 120 * flash), (int)(14 + 90 * flash), (int)(78 + 150 * flash)), 0);

  const Mat3 spin = rotation(t * 0.37f, t * 0.61f, t * 0.23f);
  const float scale = 0.92f + 0.05f * sinf(t * 1.7f);
  const float fit = fminf(Width * 0.5f, params.height * 0.5f) * 0.95f;
  const Vec3 offset = vec3(0.0f, 0.0f, fmaxf(7.8f, Focal * 3.0f / fit));
  const Vec3 light = normalize(vec3(-0.5f, 0.8f, -0.6f));
  const Vec3 half = normalize(light + vec3(0.0f, 0.0f, -1.0f));
  const float hueShift = t * 0.08f;

  for (uint16_t i = 0; i < KnotVertexCount; ++i) {
    _camera[i] = spin.apply(_knotPositions[i] * scale) + offset;
    ProjectedVertex &out = _projected[i];
    if (!project(_camera[i], cx, cy, out)) continue;
    const Vec3 n = spin.apply(_knotNormals[i]);
    const float diffuse = fmaxf(0.0f, dot(n, light));
    float spec = fmaxf(0.0f, dot(n, half));
    spec *= spec;
    spec *= spec;
    spec *= spec;
    spec *= spec;
    spec *= spec;
    float rim = 1.0f - fmaxf(0.0f, -n.z);
    rim = rim * rim * rim * (0.7f + flash);
    float r, g, b;
    hsv(_knotHue[i] + hueShift, 0.82f, 1.0f, r, g, b);
    const float lit = 0.12f + 0.88f * diffuse;
    out.r = r * lit + 240.0f * spec + 130.0f * rim + 120.0f * flash;
    out.g = g * lit + 240.0f * spec + 70.0f * rim + 120.0f * flash;
    out.b = b * lit + 255.0f * spec + 255.0f * rim + 120.0f * flash;
  }
  for (uint16_t f = 0; f < KnotTriCount; ++f) {
    const uint16_t *tri = _knotIndices + f * 3;
    const ProjectedVertex &a = _projected[tri[0]];
    const ProjectedVertex &b = _projected[tri[1]];
    const ProjectedVertex &c = _projected[tri[2]];
    if (a.depth < 0 || b.depth < 0 || c.depth < 0) continue;
    const Vec3 p0 = _camera[tri[0]];
    if (dot(cross(_camera[tri[1]] - p0, _camera[tri[2]] - p0), p0) >= 0.0f) continue;
    emitTri(a, b, c, false, 0);
  }
}

void BenchmarkScenes::setupTunnel(const BenchFrameParams &params) {
  const float t = params.t;
  _tunnelRotate = (uint8_t)(int32_t)(t * 30.0f);
  _tunnelMove = (uint8_t)(int32_t)(t * 120.0f);
  _tunnelT1 = (uint8_t)(int32_t)(t * 70.0f);
  _tunnelT2 = (uint8_t)(int32_t)(t * 45.0f);
  _tunnelT3 = (uint8_t)(int32_t)(-t * 25.0f);
  _tunnelTwist = (int16_t)(16.0f * sinf(t * 0.7f));

  const float hueBase = 0.55f + t * 0.03f;
  for (uint16_t i = 0; i < 256; ++i) {
    const float phase = i / 256.0f;
    float r, g, b;
    hsv(hueBase + 0.22f * sinf(TwoPi * phase), 0.78f, 0.55f + 0.45f * sinf(TwoPi * phase * 3.0f + t), r, g, b);
    for (uint8_t shade = 0; shade < 16; ++shade) {
      const float level = shade / 15.0f;
      _palette[(shade << 8) | i] = rgb565((int)(r * level), (int)(g * level), (int)(b * level));
    }
  }
}

void BenchmarkScenes::setupScore(const BenchFrameParams &params) {
  const float t = params.t;
  const float cx = Width * 0.5f;
  const float cy = params.height * 0.5f;
  buildGlow(rgb565(48, 30, 86), 0);

  static const uint8_t SegmentMasks[11] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F, 0x00};
  // Segment boxes a..g as {minX, minY, maxX, maxY} in a 1.0 x 1.8 digit cell.
  static const float Segments[7][4] = {
    {-0.38f, 0.70f, 0.38f, 0.90f},   {0.30f, 0.06f, 0.50f, 0.74f},   {0.30f, -0.74f, 0.50f, -0.06f},
    {-0.38f, -0.90f, 0.38f, -0.70f}, {-0.50f, -0.74f, -0.30f, -0.06f}, {-0.50f, 0.06f, -0.30f, 0.74f},
    {-0.38f, -0.10f, 0.38f, 0.10f}};
  static const uint8_t Faces[6][4] = {{0, 2, 6, 4}, {1, 3, 7, 5}, {0, 1, 5, 4},
                                      {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 5, 7, 6}};
  static const Vec3 FaceNormals[6] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};

  const uint8_t digits = params.digits < 1 ? 1 : (params.digits > 5 ? 5 : params.digits);
  // 10 marks a blank leading position (all segments unlit).
  uint8_t values[5] = {10, 10, 10, 10, 10};
  uint32_t value = params.value;
  for (int8_t i = digits - 1; i >= 0; --i) {
    values[i] = value % 10;
    value /= 10;
    if (!value) break;
  }

  const float e = easeOutCubic(t / 1.6f);
  const float spacing = 1.3f;
  const float total = digits * spacing - 0.3f;
  const float fitWidth = Focal * (total * 0.5f + 0.3f) / 150.0f;
  const float fitHeight = Focal * 1.0f / (params.height * 0.46f);
  const float distance = fmaxf(fitWidth, fitHeight) + 0.4f;
  const float size = 0.65f + 0.35f * e;
  const Mat3 spin = rotation(0.18f, -0.28f + 2.4f * (1.0f - e), 0.0f);
  const Vec3 offset = vec3(0.0f, 0.0f, distance);
  const Vec3 light = normalize(vec3(0.4f, 0.6f, -0.7f));
  const Vec3 half = normalize(light + vec3(0.0f, 0.0f, -1.0f));
  const float sweep = -total * 0.5f - 1.0f + (t - 0.8f) * 6.0f;
  const float depthHalf = 0.14f;

  for (uint8_t d = 0; d < digits; ++d) {
    const float digitX = -total * 0.5f + 0.5f + d * spacing;
    const uint8_t mask = SegmentMasks[values[d]];
    for (uint8_t s = 0; s < 7; ++s) {
      const bool lit = (mask >> s) & 1;
      const float *box = Segments[s];
      Vec3 corners[8];
      ProjectedVertex projected[8];
      bool visible = true;
      for (uint8_t c = 0; c < 8; ++c) {
        const Vec3 local = vec3((digitX + ((c & 1) ? box[2] : box[0])) * size,
                                ((c & 2) ? box[3] : box[1]) * size, ((c & 4) ? depthHalf : -depthHalf) * size);
        corners[c] = spin.apply(local) + offset;
        visible = project(corners[c], cx, cy, projected[c]) && visible;
      }
      if (!visible) continue;
      const float highlight = lit ? fmaxf(0.0f, 1.0f - fabsf(digitX - sweep) * 1.2f) : 0.0f;
      for (uint8_t f = 0; f < 6; ++f) {
        const Vec3 n = spin.apply(FaceNormals[f]);
        const Vec3 &p = corners[Faces[f][0]];
        if (dot(n, p) >= 0.0f) continue;
        const float diffuse = fmaxf(0.0f, dot(n, light));
        float spec = fmaxf(0.0f, dot(n, half));
        spec *= spec;
        spec *= spec;
        spec *= spec;
        spec *= spec;
        spec *= spec;
        float r, g, b;
        if (lit) {
          const float shade = 0.35f + 0.65f * diffuse;
          r = 255.0f * shade + 120.0f * spec + 200.0f * highlight;
          g = 170.0f * shade + 110.0f * spec + 200.0f * highlight;
          b = 40.0f * shade + 80.0f * spec + 200.0f * highlight;
        } else {
          const float shade = 0.5f + 0.5f * diffuse;
          r = 34.0f * shade;
          g = 26.0f * shade;
          b = 62.0f * shade;
        }
        const uint16_t color = rgb565((int)r, (int)g, (int)b);
        const uint8_t *q = Faces[f];
        emitTri(projected[q[0]], projected[q[1]], projected[q[2]], true, color);
        emitTri(projected[q[0]], projected[q[2]], projected[q[3]], true, color);
      }
    }
  }
}

void BenchmarkScenes::renderBand(BenchScene scene, const Band &band) const {
  switch (scene) {
    case BenchScene::Warp:
      drawGlow(band);
      drawSegments(band);
      break;
    case BenchScene::Crystal:
      drawSkyAndFloor(band);
      clearDepth(band);
      drawTris(band, _tris, _triY, _triCount);
      break;
    case BenchScene::Knot:
    case BenchScene::Score:
      drawGlow(band);
      clearDepth(band);
      drawTris(band, _tris, _triY, _triCount);
      break;
    case BenchScene::Tunnel: drawTunnel(band); break;
    default: memset(band.pixels, 0, (size_t)band.rows * Width * sizeof(uint16_t)); break;
  }
}

void BenchmarkScenes::drawGlow(const Band &band) const {
  const int32_t cy2 = _height;  // centre in half-pixels
  for (int16_t row = 0; row < band.rows; ++row) {
    const int32_t dyHalf = (band.y0 + row) * 2 + 1 - cy2;
    const int32_t dy2 = (dyHalf * dyHalf) >> 2;
    uint16_t *pixel = band.pixels + (int32_t)row * Width;
    for (int16_t x = 0; x < Width; ++x) {
      uint32_t index = (uint32_t)(_dx2[x] + dy2) >> 6;
      if (index >= GlowEntries) index = GlowEntries - 1;
      pixel[x] = _glow[index];
    }
  }
}

void BenchmarkScenes::drawSkyAndFloor(const Band &band) const {
  const float cx = Width * 0.5f;
  const float radius2 = _sunRadius * _sunRadius;
  const uint16_t sunTop = rgb565(255, 225, 90);
  const uint16_t sunBottom = rgb565(255, 50, 150);
  for (int16_t row = 0; row < band.rows; ++row) {
    const int16_t y = band.y0 + row;
    uint16_t *pixel = band.pixels + (int32_t)row * Width;
    const uint16_t base = _rowColors[y];
    if (y < _horizon) {
      for (int16_t x = 0; x < Width; ++x) pixel[x] = base;
      const float dy = y + 0.5f - _sunCenterY;
      if (dy * dy >= radius2) continue;
      const float s = (dy + _sunRadius) / (2.0f * _sunRadius);
      // Widening horizontal gaps scroll slowly up through the lower half.
      if (s > 0.55f) {
        const float lower = (s - 0.55f) / 0.45f;
        const float stripe = lower * 7.0f + _sunPhase;
        if (stripe - floorf(stripe) < lower * 0.6f) continue;
      }
      const float halfWidth = sqrtf(radius2 - dy * dy);
      int16_t xs = (int16_t)(cx - halfWidth + 0.5f);
      int16_t xe = (int16_t)(cx + halfWidth + 0.5f);
      if (xs < 0) xs = 0;
      if (xe > Width) xe = Width;
      const uint16_t color = lerp565(sunTop, sunBottom, (uint8_t)(clamp01(s) * 255.0f));
      for (int16_t x = xs; x < xe; ++x) pixel[x] = color;
      if (xs > 0) pixel[xs - 1] = lerp565(base, color, 128);
      if (xe < Width) pixel[xe] = lerp565(base, color, 128);
      continue;
    }
    const uint16_t line = _rowLineColors[y];
    const int32_t scale = _rowLineScale[y];
    const int32_t lineZ = _rowLineZ[y];
    int32_t world = _rowWorldX0[y] + _floorSway;
    const int32_t step = _rowWorldStep[y];
    for (int16_t x = 0; x < Width; ++x) {
      const uint32_t fraction = (uint32_t)world & 0xFFFF;
      const int32_t distance = fraction < 32768 ? (int32_t)fraction : (int32_t)(65536 - fraction);
      int32_t intensity = 255 - ((distance * scale) >> 16);
      if (intensity < lineZ) intensity = lineZ;
      pixel[x] = intensity <= 0 ? base : lerp565(base, line, (uint8_t)(intensity > 255 ? 255 : intensity));
      world += step;
    }
  }
}

void BenchmarkScenes::drawTunnel(const Band &band) const {
  if (!_tunnel) return;
  const uint8_t rotate = _tunnelRotate, move = _tunnelMove;
  const uint8_t t1 = _tunnelT1, t2 = _tunnelT2, t3 = _tunnelT3;
  const int16_t twist = _tunnelTwist;
  for (int16_t row = 0; row < band.rows; ++row) {
    const uint32_t *lut = _tunnel + (int32_t)(band.y0 + row) * Width;
    uint16_t *pixel = band.pixels + (int32_t)row * Width;
    for (int16_t x = 0; x < Width; ++x) {
      const uint32_t entry = lut[x];
      const uint8_t depth = (uint8_t)(entry >> 8);
      const uint8_t u = (uint8_t)(entry + rotate + ((depth * twist) >> 4));
      const uint8_t v = (uint8_t)(depth + move);
      uint8_t index = (uint8_t)(_sine[(uint8_t)(u + t1)] + _sine[(uint8_t)(v * 2 + t2)] +
                                _sine[(uint8_t)(u + v + t3)]);
      if ((u ^ v) & 0x20) index += 40;
      pixel[x] = _palette[((entry >> 8) & 0xF00) | index];
    }
  }
}

void BenchmarkScenes::drawSegments(const Band &band) const {
  const int16_t bandEnd = band.y0 + band.rows;
  for (uint16_t i = 0; i < _segmentCount; ++i) {
    const Segment &segment = _segments[i];
    if (segment.yMax < band.y0 || segment.yMin >= bandEnd) continue;
    const int32_t dx = segment.x1 - segment.x0;
    const int32_t dy = segment.y1 - segment.y0;
    int32_t steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    if (steps < 1) steps = 1;
    if (steps > 600) steps = 600;
    int32_t first = 0, last = steps;
    if (dy != 0) {
      int32_t a = (int32_t)(band.y0 - segment.y0) * steps / dy;
      int32_t b = (int32_t)(bandEnd - segment.y0) * steps / dy;
      if (a > b) { const int32_t swap = a; a = b; b = swap; }
      first = max<int32_t>(0, a - 1);
      last = min<int32_t>(steps, b + 1);
    }
    const int32_t xStep = dx * 65536 / steps;
    const int32_t yStep = dy * 65536 / steps;
    int32_t xf = (int32_t)segment.x0 * 65536 + 0x8000 + xStep * first;
    int32_t yf = (int32_t)segment.y0 * 65536 + 0x8000 + yStep * first;
    for (int32_t s = first; s <= last; ++s, xf += xStep, yf += yStep) {
      const int16_t x = (int16_t)(xf >> 16);
      const int16_t y = (int16_t)(yf >> 16);
      if (x < 0 || x >= Width || y < band.y0 || y >= bandEnd) continue;
      const uint16_t amount = (uint16_t)(48 + 208 * s / steps);
      uint16_t &pixel = band.pixels[(int32_t)(y - band.y0) * Width + x];
      pixel = addSaturate(pixel, scale565(segment.color, amount));
    }
    if (segment.headSize > 1) {
      for (int16_t oy = 0; oy < 2; ++oy) {
        const int16_t y = segment.y1 + oy;
        if (y < band.y0 || y >= bandEnd) continue;
        for (int16_t ox = 0; ox < 2; ++ox) {
          const int16_t x = segment.x1 + ox;
          if (x < 0 || x >= Width) continue;
          uint16_t &pixel = band.pixels[(int32_t)(y - band.y0) * Width + x];
          pixel = addSaturate(pixel, segment.color);
        }
      }
    }
  }
}
