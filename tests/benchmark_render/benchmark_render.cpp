// Host check for the Settings benchmark renderer: every scene draws visible
// content, rendering bands in reverse order produces the identical frame (the
// property the two-core split relies on), and PPM previews are written for
// visual review.
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "../../src/features/settings/benchmark/BenchmarkScenes.h"

namespace {

int failures = 0;

void check(bool condition, const std::string &what) {
  if (!condition) {
    ++failures;
    printf("FAIL: %s\n", what.c_str());
  } else {
    printf("ok: %s\n", what.c_str());
  }
}

void render(BenchmarkScenes &scenes, BenchScene scene, const BenchFrameParams &params,
            std::vector<uint16_t> &frame, bool reverse) {
  frame.assign((size_t)BenchRaster::Width * params.height, 0xF81F);
  std::vector<uint16_t> depth((size_t)BenchRaster::Width * BenchRaster::BandHeight);
  scenes.setupFrame(scene, params);
  const int bands = (params.height + BenchRaster::BandHeight - 1) / BenchRaster::BandHeight;
  for (int i = 0; i < bands; ++i) {
    const int band = reverse ? bands - 1 - i : i;
    BenchRaster::Band target;
    target.y0 = band * BenchRaster::BandHeight;
    target.rows = std::min<int>(BenchRaster::BandHeight, params.height - target.y0);
    target.pixels = frame.data() + (size_t)target.y0 * BenchRaster::Width;
    target.depth = depth.data();
    scenes.renderBand(scene, target);
  }
}

void writePpm(const std::string &path, const std::vector<uint16_t> &frame, int height) {
  FILE *file = fopen(path.c_str(), "wb");
  if (!file) return;
  fprintf(file, "P6\n%d %d\n255\n", BenchRaster::Width, height);
  for (uint16_t pixel : frame) {
    const uint8_t rgb[3] = {(uint8_t)(((pixel >> 11) & 0x1F) * 255 / 31),
                            (uint8_t)(((pixel >> 5) & 0x3F) * 255 / 63), (uint8_t)((pixel & 0x1F) * 255 / 31)};
    fwrite(rgb, 1, 3, file);
  }
  fclose(file);
}

struct Case {
  const char *name;
  BenchScene scene;
  int16_t height;
  float t;
  float flash;
  uint32_t value;
  uint8_t digits;
};

}  // namespace

int main(int argc, char **argv) {
  const std::string outDir = argc > 1 ? argv[1] : ".";
  BenchmarkScenes scenes;
  check(scenes.begin(), "scene buffers allocate");

  const Case cases[] = {
    {"warp_1s", BenchScene::Warp, 384, 1.0f, 0.0f, 0, 1},
    {"warp_7s", BenchScene::Warp, 384, 7.0f, 0.0f, 0, 1},
    {"crystal_2s", BenchScene::Crystal, 384, 2.0f, 0.0f, 0, 1},
    {"crystal_8s", BenchScene::Crystal, 384, 8.0f, 0.0f, 0, 1},
    {"knot_3s", BenchScene::Knot, 384, 3.0f, 0.0f, 0, 1},
    {"knot_flash", BenchScene::Knot, 384, 7.1f, 0.8f, 0, 1},
    {"knot_preview", BenchScene::Knot, 192, 2.2f, 0.0f, 0, 1},
    {"tunnel_2s", BenchScene::Tunnel, 384, 2.0f, 0.0f, 0, 1},
    {"tunnel_9s", BenchScene::Tunnel, 384, 9.0f, 0.0f, 0, 1},
    {"score_mid", BenchScene::Score, 144, 0.6f, 0.0f, 731, 4},
    {"score_final", BenchScene::Score, 144, 2.4f, 0.0f, 1042, 4},
    {"score_5", BenchScene::Score, 144, 2.4f, 0.0f, 12873, 5},
  };

  for (const Case &item : cases) {
    check(scenes.prepare(item.scene, item.height), std::string(item.name) + " prepares");
    BenchFrameParams params;
    params.t = item.t;
    params.flash = item.flash;
    params.height = item.height;
    params.value = item.value;
    params.digits = item.digits;
    std::vector<uint16_t> forward, backward;
    render(scenes, item.scene, params, forward, false);
    render(scenes, item.scene, params, backward, true);
    check(forward == backward, std::string(item.name) + " is independent of band order");
    size_t lit = 0, untouched = 0;
    for (uint16_t pixel : forward) {
      if (pixel == 0xF81F) ++untouched;
      if (((pixel >> 11) & 0x1F) + ((pixel >> 5) & 0x3F) / 2 + (pixel & 0x1F) > 12) ++lit;
    }
    check(untouched < forward.size() / 100, std::string(item.name) + " paints the whole canvas");
    check(lit > forward.size() / 20, std::string(item.name) + " draws visible content");
    writePpm(outDir + "/" + item.name + ".ppm", forward, item.height);
  }
  scenes.end();
  printf("%s\n", failures ? "FAILED" : "PASSED");
  return failures ? 1 : 0;
}
