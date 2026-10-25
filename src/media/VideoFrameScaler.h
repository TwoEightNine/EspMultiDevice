#pragma once

#include <Arduino.h>

#include "VideoH264.h"

struct VideoColorTables;

// Converts decoder-owned planar I420 into packed little-endian RGB565 with
// BT.601 limited-range nearest-neighbour scaling. Output is bit-identical to
// the direct per-pixel formula. Its lookups live in internal RAM when
// available; it is used only by the video worker task.
class VideoFrameScaler {
public:
  ~VideoFrameScaler();

  // Prepares lookups for one source width and packed destination width.
  bool prepare(uint16_t sourceWidth, uint16_t destinationWidth);
  // `frame.width` must equal the prepared source width and `width` the
  // prepared destination width.
  void convert(const VideoH264::Frame &frame, uint16_t *destination, uint16_t width, uint16_t height) const;
  void end();

private:
  VideoColorTables *_tables = nullptr;
  uint16_t *_columns = nullptr;
  uint16_t _columnCount = 0;

  void convertNative(const VideoH264::Frame &frame, uint16_t *destination, uint16_t width, uint16_t height) const;
};
