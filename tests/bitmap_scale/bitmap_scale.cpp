// Host contract test for BitmapScale: aspect-fit sizing, never upscaling,
// the JPEG pre-scale choice, and the row-by-row box reducer.
#include <stdio.h>

#include <string>
#include <vector>

#include "../../src/media/BitmapScale.h"

namespace {
int failures = 0;
void check(bool condition, const std::string &what) {
  if (!condition) { ++failures; printf("FAIL: %s\n", what.c_str()); }
  else printf("ok: %s\n", what.c_str());
}

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

void fitIs(uint16_t w, uint16_t h, uint16_t mw, uint16_t mh, uint16_t ew, uint16_t eh) {
  uint16_t ow = 0, oh = 0;
  BitmapScale::fit(w, h, mw, mh, ow, oh);
  char text[160];
  snprintf(text, sizeof(text), "fit %ux%u into %ux%u gives %ux%u (got %ux%u)", w, h, mw, mh, ew, eh, ow, oh);
  check(ow == ew && oh == eh, text);
}

std::vector<uint16_t> reduce(const std::vector<uint16_t> &source, uint16_t sw, uint16_t sh,
                             uint16_t tw, uint16_t th) {
  std::vector<uint16_t> target(static_cast<size_t>(tw) * th, 0xffff);
  BitmapScale::Reducer reducer;
  if (!reducer.begin(sw, sh, target.data(), tw, th)) return {};
  for (uint16_t y = 0; y < sh; ++y) reducer.addRow(y, source.data() + static_cast<size_t>(y) * sw);
  reducer.finish();
  return target;
}
}  // namespace

int main() {
  // Aspect ratio and no upscaling.
  fitIs(216, 250, 100, 100, 86, 100);
  fitIs(250, 216, 100, 100, 100, 86);
  fitIs(250, 250, 100, 100, 100, 100);
  fitIs(80, 60, 100, 100, 80, 60);
  fitIs(100, 100, 100, 100, 100, 100);
  fitIs(1000, 3, 100, 100, 100, 1);
  fitIs(3, 1000, 100, 100, 1, 100);
  fitIs(10168, 5000, 320, 432, 320, 157);
  fitIs(201, 99, 100, 100, 100, 49);
  fitIs(1, 1, 100, 100, 1, 1);

  // JPEG pre-scale: largest reduction that still covers the box.
  check(BitmapScale::jpegShift(216, 250, 86, 100) == 1, "216x250 to 86x100 decodes at 1/2");
  check(BitmapScale::jpegShift(250, 216, 100, 86) == 1, "250x216 to 100x86 decodes at 1/2");
  check(BitmapScale::jpegShift(1600, 1200, 100, 75) == 3, "1600x1200 to 100x75 decodes at 1/8");
  check(BitmapScale::jpegShift(800, 600, 100, 75) == 3, "800x600 to 100x75 decodes at 1/8 exactly");
  check(BitmapScale::jpegShift(799, 600, 100, 75) == 3, "an odd width rounds up and still covers");
  check(BitmapScale::jpegShift(790, 590, 100, 75) == 2, "a slightly smaller source decodes at 1/4");
  check(BitmapScale::jpegShift(100, 100, 100, 100) == 0, "no reduction without downscaling");
  check(BitmapScale::reduced(201, 1) == 101 && BitmapScale::reduced(7, 3) == 1, "reduced sizes round up");

  // A uniform image stays uniform at any reduction.
  {
    const uint16_t color = rgb(17, 40, 9);
    std::vector<uint16_t> source(216 * 250, color);
    const std::vector<uint16_t> out = reduce(source, 216, 250, 86, 100);
    bool same = out.size() == 86 * 100;
    for (uint16_t value : out) same = same && value == color;
    check(same, "uniform 216x250 reduces to a uniform 86x100");
  }
  // 2x2 blocks average exactly.
  {
    std::vector<uint16_t> source = {rgb(0, 0, 0), rgb(31, 63, 31), rgb(4, 8, 2), rgb(4, 8, 2),
                                    rgb(31, 63, 31), rgb(0, 0, 0), rgb(4, 8, 2), rgb(4, 8, 2)};
    const std::vector<uint16_t> out = reduce(source, 4, 2, 2, 1);
    check(out.size() == 2 && out[0] == rgb(16, 32, 16) && out[1] == rgb(4, 8, 2),
          "2x2 boxes average each channel with rounding");
  }
  // Odd sizes: every target pixel is written exactly once.
  {
    for (uint16_t sw : {7, 13, 99, 101}) {
      for (uint16_t tw : {1, 3, 6}) {
        std::vector<uint16_t> source(static_cast<size_t>(sw) * 5, rgb(1, 2, 3));
        const std::vector<uint16_t> out = reduce(source, sw, 5, tw, 3);
        bool written = !out.empty();
        for (uint16_t value : out) written = written && value == rgb(1, 2, 3);
        char text[96];
        snprintf(text, sizeof(text), "%ux5 to %ux3 writes every target pixel", sw, tw);
        check(written, text);
      }
    }
  }
  // Identity size copies the source.
  {
    std::vector<uint16_t> source(12);
    for (size_t i = 0; i < source.size(); ++i) source[i] = static_cast<uint16_t>(i * 4099);
    check(reduce(source, 4, 3, 4, 3) == source, "same-size reduction copies the source");
  }
  // Upscaling and empty targets are refused; out-of-order rows are ignored.
  {
    uint16_t target[4] = {};
    BitmapScale::Reducer reducer;
    check(!reducer.begin(2, 2, target, 4, 1), "a wider target is refused");
    check(!reducer.begin(2, 2, target, 0, 1), "an empty target is refused");
    check(reducer.begin(2, 2, target, 1, 1), "a 2x2 to 1x1 reduction starts");
    const uint16_t first[2] = {rgb(10, 0, 0), rgb(10, 0, 0)};
    const uint16_t second[2] = {rgb(20, 0, 0), rgb(20, 0, 0)};
    reducer.addRow(1, second);
    reducer.addRow(0, first);  // Out of order: ignored.
    reducer.finish();
    check(target[0] == rgb(20, 0, 0), "rows that arrive out of order are ignored");
  }

  if (failures) { printf("%d check(s) failed\n", failures); return 1; }
  printf("bitmap_scale: all checks passed\n");
  return 0;
}
