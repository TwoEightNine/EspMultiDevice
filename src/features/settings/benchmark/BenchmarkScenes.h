#pragma once

#include <Arduino.h>

#include "BenchmarkRaster.h"

enum class BenchScene : uint8_t { None, Warp, Crystal, Knot, Tunnel, Score };

struct BenchFrameParams {
  float t = 0.0f;        // seconds into the scene's animation
  float flash = 0.0f;    // 0..1 highlight pulse
  int16_t height = 0;    // canvas rows (always BenchRaster::Width columns)
  uint32_t value = 0;    // Score scene: number to show
  uint8_t digits = 1;    // Score scene: digit count of the final score
};

// Procedural scene content for the Settings benchmark. setupFrame() runs on
// one worker and writes the shared per-frame state; renderBand() may then run
// concurrently on both workers for different bands and only reads that state.
class BenchmarkScenes {
public:
  ~BenchmarkScenes();

  bool begin();
  void end();
  // Allocates scene-specific tables for the canvas height. Untimed.
  bool prepare(BenchScene scene, int16_t height);
  void setupFrame(BenchScene scene, const BenchFrameParams &params);
  void renderBand(BenchScene scene, const BenchRaster::Band &band) const;

private:
  struct Segment {
    int16_t x0, y0, x1, y1;
    int16_t yMin, yMax;
    uint16_t color;
    uint8_t headSize;
  };

  // Shared triangle pipeline.
  BenchRaster::Tri *_tris = nullptr;
  int16_t *_triY = nullptr;
  uint16_t _triCount = 0;
  BenchRaster::Vec3 *_camera = nullptr;
  BenchRaster::ProjectedVertex *_projected = nullptr;

  // Torus knot mesh.
  BenchRaster::Vec3 *_knotPositions = nullptr;
  BenchRaster::Vec3 *_knotNormals = nullptr;
  float *_knotHue = nullptr;
  uint16_t *_knotIndices = nullptr;
  bool _knotReady = false;

  // Crystal mesh (displaced icosphere).
  BenchRaster::Vec3 *_crystalPositions = nullptr;
  uint16_t *_crystalIndices = nullptr;
  uint16_t _crystalVertexCount = 0;
  uint16_t _crystalTriCount = 0;

  // Warp stars.
  Segment *_segments = nullptr;
  uint16_t _segmentCount = 0;

  // Radial glow shared by Warp, Knot, and Score backgrounds.
  uint8_t *_falloff = nullptr;
  uint16_t *_glow = nullptr;
  int32_t _dx2[BenchRaster::Width];

  // Crystal sky, sun, and floor.
  uint16_t *_rowColors = nullptr;
  uint16_t *_rowLineColors = nullptr;
  int32_t *_rowLineScale = nullptr;
  uint8_t *_rowLineZ = nullptr;
  int32_t *_rowWorldX0 = nullptr;
  int32_t *_rowWorldStep = nullptr;
  int16_t _horizon = 0;
  int32_t _floorSway = 0;
  float _sunCenterY = 0.0f;
  float _sunRadius = 0.0f;
  float _sunPhase = 0.0f;

  // Plasma tunnel.
  uint32_t *_tunnel = nullptr;
  uint16_t *_palette = nullptr;  // 16 shades x 256 entries
  uint8_t _sine[256];
  uint8_t _tunnelRotate = 0, _tunnelMove = 0, _tunnelT1 = 0, _tunnelT2 = 0, _tunnelT3 = 0;
  int16_t _tunnelTwist = 0;
  int16_t _tunnelHeight = 0;

  int16_t _height = 0;

  bool buildKnot();
  bool buildCrystal();
  bool buildTunnel(int16_t height);
  void buildFloorRows(int16_t height);
  void buildGlow(uint16_t inner, uint16_t outer);

  void setupWarp(const BenchFrameParams &params);
  void setupCrystal(const BenchFrameParams &params);
  void setupKnot(const BenchFrameParams &params);
  void setupTunnel(const BenchFrameParams &params);
  void setupScore(const BenchFrameParams &params);
  bool emitTri(const BenchRaster::ProjectedVertex &a, const BenchRaster::ProjectedVertex &b,
               const BenchRaster::ProjectedVertex &c, bool flat, uint16_t color);

  void drawGlow(const BenchRaster::Band &band) const;
  void drawSkyAndFloor(const BenchRaster::Band &band) const;
  void drawTunnel(const BenchRaster::Band &band) const;
  void drawSegments(const BenchRaster::Band &band) const;
};
