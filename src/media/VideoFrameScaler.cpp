// The per-pixel loops dominate video conversion; the rest of the sketch keeps
// the Arduino default -Os.
#if defined(__GNUC__)
#pragma GCC optimize("O2")
#endif

#include "VideoFrameScaler.h"

#include <esp32-hal-psram.h>
#include <esp_heap_caps.h>
#include <stdlib.h>
#include <string.h>

namespace {
// Each table maps one 8.8 fixed-point channel sum, shifted down, to its
// already-masked RGB565 bits. The offset covers the full [-278, 535] range
// reachable from 8-bit Y/U/V, so the pixel loop needs no clamping branches.
constexpr int ColorTableOffset = 384;
constexpr int ColorTableSize = 1024;
}  // namespace

struct VideoColorTables {
  uint16_t red[ColorTableSize];
  uint16_t green[ColorTableSize];
  uint16_t blue[ColorTableSize];
};

namespace {
struct ChromaTerms {
  int red;
  int green;
  int blue;
};

void buildColorTables(VideoColorTables &tables) {
  for (int index = 0; index < ColorTableSize; ++index) {
    const int value = index - ColorTableOffset;
    const uint8_t channel = value < 0 ? 0 : (value > 255 ? 255 : (uint8_t)value);
    tables.red[index] = (uint16_t)(channel & 0xf8) << 8;
    tables.green[index] = (uint16_t)(channel & 0xfc) << 3;
    tables.blue[index] = channel >> 3;
  }
}

inline ChromaTerms chromaTerms(uint8_t u, uint8_t v) {
  const int d = u - 128;
  const int e = v - 128;
  return {409 * e, -100 * d - 208 * e, 516 * d};
}

inline uint16_t packPixel(const VideoColorTables &tables, uint8_t y, const ChromaTerms &chroma) {
  // 298 * (Y - 16) plus the rounding term shared by all three channels.
  const int luma = 298 * (y - 16) + 128;
  return tables.red[((luma + chroma.red) >> 8) + ColorTableOffset] |
         tables.green[((luma + chroma.green) >> 8) + ColorTableOffset] |
         tables.blue[((luma + chroma.blue) >> 8) + ColorTableOffset];
}

void *allocateLookup(size_t bytes) {
  void *memory = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return memory ? memory : ps_malloc(bytes);
}
}  // namespace

VideoFrameScaler::~VideoFrameScaler() {
  end();
}

bool VideoFrameScaler::prepare(uint16_t sourceWidth, uint16_t destinationWidth) {
  if (!sourceWidth || !destinationWidth) return false;
  if (!_tables) {
    _tables = static_cast<VideoColorTables *>(allocateLookup(sizeof(VideoColorTables)));
    if (!_tables) return false;
    buildColorTables(*_tables);
  }
  if (_columnCount != destinationWidth) {
    free(_columns);
    _columnCount = 0;
    _columns = static_cast<uint16_t *>(allocateLookup((size_t)destinationWidth * sizeof(uint16_t)));
    if (!_columns) return false;
    _columnCount = destinationWidth;
  }
  // 16.16 nearest-neighbour mapping, matching the per-row source mapping.
  const uint32_t xStep = ((uint32_t)sourceWidth << 16) / destinationWidth;
  uint32_t sourceXFixed = 0;
  for (uint16_t x = 0; x < destinationWidth; ++x) {
    _columns[x] = (uint16_t)min<uint32_t>(sourceWidth - 1, sourceXFixed >> 16);
    sourceXFixed += xStep;
  }
  return true;
}

void VideoFrameScaler::end() {
  free(_tables);
  _tables = nullptr;
  free(_columns);
  _columns = nullptr;
  _columnCount = 0;
}

void VideoFrameScaler::convert(const VideoH264::Frame &frame, uint16_t *destination, uint16_t width,
                               uint16_t height) const {
  if (!_tables || !_columns || width != _columnCount || !height) return;
  if (frame.width == width && frame.height == height) {
    convertNative(frame, destination, width, height);
    return;
  }
  const VideoColorTables &tables = *_tables;
  const uint16_t *columns = _columns;
  const uint32_t sourceH = frame.height;
  const uint32_t yStep = (sourceH << 16) / height;
  uint32_t sourceYFixed = 0;
  uint32_t previousSourceY = UINT32_MAX;
  for (uint32_t y = 0; y < height; ++y) {
    const uint32_t sourceY = min<uint32_t>(sourceH - 1, sourceYFixed >> 16);
    sourceYFixed += yStep;
    uint16_t *output = destination + y * width;
    if (sourceY == previousSourceY) {
      // An upscaled row repeats the previous source row exactly.
      memcpy(output, output - width, width * sizeof(uint16_t));
      continue;
    }
    previousSourceY = sourceY;
    const uint8_t *luma = frame.y + sourceY * frame.yStride;
    const uint8_t *u = frame.u + (sourceY / 2) * frame.uStride;
    const uint8_t *v = frame.v + (sourceY / 2) * frame.vStride;
    // Adjacent destination columns usually share a chroma sample; compute its
    // terms once per run rather than once per pixel.
    uint32_t chromaX = UINT32_MAX;
    ChromaTerms chroma{};
    for (uint32_t x = 0; x < width; ++x) {
      const uint32_t sourceX = columns[x];
      if ((sourceX >> 1) != chromaX) {
        chromaX = sourceX >> 1;
        chroma = chromaTerms(u[chromaX], v[chromaX]);
      }
      output[x] = packPixel(tables, luma[sourceX], chroma);
    }
  }
}

// Native-size frames convert in 2x2 blocks that share one chroma sample.
void VideoFrameScaler::convertNative(const VideoH264::Frame &frame, uint16_t *destination, uint16_t width,
                                     uint16_t height) const {
  const VideoColorTables &tables = *_tables;
  const uint32_t evenWidth = width & ~1U;
  for (uint32_t y = 0; y < height; y += 2) {
    const bool secondRow = y + 1 < height;
    const uint8_t *luma0 = frame.y + y * frame.yStride;
    const uint8_t *luma1 = luma0 + frame.yStride;
    const uint8_t *u = frame.u + (y / 2) * frame.uStride;
    const uint8_t *v = frame.v + (y / 2) * frame.vStride;
    uint16_t *output0 = destination + y * width;
    uint16_t *output1 = output0 + width;
    if (secondRow) {
      for (uint32_t x = 0; x < evenWidth; x += 2) {
        const ChromaTerms chroma = chromaTerms(u[x / 2], v[x / 2]);
        output0[x] = packPixel(tables, luma0[x], chroma);
        output0[x + 1] = packPixel(tables, luma0[x + 1], chroma);
        output1[x] = packPixel(tables, luma1[x], chroma);
        output1[x + 1] = packPixel(tables, luma1[x + 1], chroma);
      }
    } else {
      for (uint32_t x = 0; x < evenWidth; x += 2) {
        const ChromaTerms chroma = chromaTerms(u[x / 2], v[x / 2]);
        output0[x] = packPixel(tables, luma0[x], chroma);
        output0[x + 1] = packPixel(tables, luma0[x + 1], chroma);
      }
    }
    if (evenWidth != width) {
      const ChromaTerms chroma = chromaTerms(u[evenWidth / 2], v[evenWidth / 2]);
      output0[evenWidth] = packPixel(tables, luma0[evenWidth], chroma);
      if (secondRow) output1[evenWidth] = packPixel(tables, luma1[evenWidth], chroma);
    }
  }
}
