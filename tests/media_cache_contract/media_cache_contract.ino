#include <Arduino.h>

#include "../../src/media/CachePath.h"
#include "../../src/media/MediaCache.h"
#include "../../src/media/SourceFingerprint.h"

namespace {
struct PathCase {
  const char *source;
  FileFormats::Format format;
  const char *directory;
  const char *finalPath;
  const char *temporaryPath;
};

const PathCase PathCases[] = {
  {"/Photo.JPG", FileFormats::Format::Eth1, "/.cache", "/.cache/Photo.JPG.eth1", "/.cache/Photo.JPG.eth1.tmp"},
  {"/media/song.MP3", FileFormats::Format::Empa, "/media/.cache", "/media/.cache/song.MP3.empa", "/media/.cache/song.MP3.empa.tmp"},
  {"/.configs/abcd", FileFormats::Format::Em16, "/.configs/.cache", "/.configs/.cache/abcd.em16", "/.configs/.cache/abcd.em16.tmp"},
  {"/.configs/abcd", FileFormats::Format::Em16Zoom, "/.configs/.cache", "/.configs/.cache/abcd.em16z", "/.configs/.cache/abcd.em16z.tmp"},
  {"clip.v1.MP4", FileFormats::Format::Emga, "/.cache", "/.cache/clip.v1.MP4.emga", "/.cache/clip.v1.MP4.emga.tmp"}
};

struct FingerprintCase {
  FileFormats::Format format;
  uint8_t offset;
};

const FingerprintCase FingerprintCases[] = {
  {FileFormats::Format::Eth1, 8},
  {FileFormats::Format::Empa, 8},
  {FileFormats::Format::Em16, 12},
  {FileFormats::Format::Em16Zoom, 12},
  {FileFormats::Format::Emga, 12}
};

uint16_t failures = 0;

void expect(bool condition, const __FlashStringHelper *message) {
  if (condition) return;
  failures++;
  Serial.println(message);
}

uint32_t readLe32(const uint8_t *bytes) {
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
         ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}
}  // namespace

void setup() {
  Serial.begin(115200);
  for (const PathCase &test : PathCases) {
    CachePaths paths;
    expect(CachePath::forSource(test.source, test.format, paths), F("path construction failed"));
    expect(paths.directory == test.directory, F("cache directory mismatch"));
    expect(paths.finalPath == test.finalPath, F("final cache path mismatch"));
    expect(paths.temporaryPath == test.temporaryPath, F("temporary cache path mismatch"));
  }

  CachePaths invalid;
  expect(!CachePath::forSource("/photo.jpg", FileFormats::Format::Jpeg, invalid),
         F("source format accepted as cache format"));
  expect(FileFormats::internalCacheCount() == 5, F("internal cache count mismatch"));
  expect(FileFormats::fromSignature((const uint8_t *)"ETH1", 4) == FileFormats::Format::Eth1,
         F("ETH1 signature not recognized"));
  for (uint8_t index = 0; index < FileFormats::internalCacheCount(); index++) {
    const FileFormats::Format format = FileFormats::internalCacheAt(index);
    expect(FileFormats::isInternal(format), F("cache registry contains source format"));
    expect(!FileFormats::isOpenable(format) && !FileFormats::isDownloadable(format),
           F("internal cache exposed as source media"));
  }

  const SourceFingerprint original{1234, 0};
  expect(original.matches({1234, 0}), F("equal fingerprint rejected"));
  expect(!original.matches({1235, 0}), F("size change accepted"));
  expect(!original.matches({1234, 1}), F("timestamp change accepted"));

  const SourceFingerprint moved{0x12345678, 0x9abcdef0};
  for (const FingerprintCase &test : FingerprintCases) {
    uint8_t header[40] = {};
    expect(MediaCache::rewriteFingerprint(test.format, header, sizeof(header), moved),
           F("cache fingerprint rewrite rejected"));
    expect(readLe32(header + test.offset) == moved.size,
           F("cache fingerprint size rewrite mismatch"));
    expect(readLe32(header + test.offset + 4) == moved.modified,
           F("cache fingerprint timestamp rewrite mismatch"));
  }
  uint8_t shortHeader[15] = {};
  expect(!MediaCache::rewriteFingerprint(FileFormats::Format::Emga, shortHeader,
                                         sizeof(shortHeader), moved),
         F("short cache header accepted"));
  expect(!MediaCache::rewriteFingerprint(FileFormats::Format::Jpeg, shortHeader,
                                         sizeof(shortHeader), moved),
         F("source format accepted for fingerprint rewrite"));

  Serial.println(failures == 0 ? F("media cache contract: PASS")
                               : F("media cache contract: FAIL"));
}

void loop() {}
