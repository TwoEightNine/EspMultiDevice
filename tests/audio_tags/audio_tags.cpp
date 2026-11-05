// Host contract test for AudioTags: ID3v2.2/2.3/2.4, ID3v1, RIFF INFO,
// text encodings, picture location, source priority, incremental stepping,
// and robustness against truncated or corrupted tags.
// Fixtures come from tools/generate_audio_tag_fixtures.py.
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "../../src/media/AudioTags.h"

namespace {
int failures = 0;

void check(bool condition, const std::string &what) {
  if (!condition) { ++failures; printf("FAIL: %s\n", what.c_str()); }
  else printf("ok: %s\n", what.c_str());
}

void checkText(const char *actual, const char *expected, const std::string &what) {
  check(strcmp(actual, expected) == 0, what + " is \"" + expected + "\" (got \"" + actual + "\")");
}

std::string fixtureDir;

std::vector<uint8_t> load(const std::string &name) {
  std::vector<uint8_t> data;
  FILE *file = fopen((fixtureDir + "/" + name).c_str(), "rb");
  if (!file) {
    printf("FAIL: cannot open fixture %s\n", name.c_str());
    ++failures;
    return data;
  }
  uint8_t buffer[4096];
  size_t count;
  while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0) data.insert(data.end(), buffer, buffer + count);
  fclose(file);
  return data;
}

struct Memory {
  const std::vector<uint8_t> *data;
  unsigned reads;
};

bool readMemory(void *context, uint32_t offset, uint8_t *buffer, size_t length) {
  Memory *memory = static_cast<Memory *>(context);
  memory->reads++;
  if (offset > memory->data->size() || length > memory->data->size() - offset) return false;
  memcpy(buffer, memory->data->data() + offset, length);
  return true;
}

uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Mirrors how AudioPlayerScreen queues sources for each container.
void queueSources(AudioTags::Parser &parser, const std::vector<uint8_t> &data, bool wav) {
  if (!wav) {
    parser.addId3v2(0, (uint32_t)data.size());
    parser.addId3v1();
    return;
  }
  uint32_t infoOffset = 0, infoSize = 0, id3Offset = 0, id3Size = 0;
  if (data.size() < 12) return;
  uint32_t cursor = 12;
  while (cursor + 8 <= data.size()) {
    uint32_t size = le32(&data[cursor + 4]);
    uint32_t payload = cursor + 8;
    if (size > data.size() - payload) break;
    if (memcmp(&data[cursor], "LIST", 4) == 0 && !infoSize) { infoOffset = payload; infoSize = size; }
    if ((memcmp(&data[cursor], "id3 ", 4) == 0 || memcmp(&data[cursor], "ID3 ", 4) == 0) && !id3Size) {
      id3Offset = payload;
      id3Size = size;
    }
    cursor = payload + size + (size & 1U);
  }
  if (id3Size) parser.addId3v2(id3Offset, id3Size);
  if (infoSize) parser.addRiffInfo(infoOffset, infoSize);
}

AudioTags::Info parse(const std::vector<uint8_t> &data, bool wav, uint16_t budget = 1000,
                      unsigned *steps = nullptr) {
  Memory memory{&data, 0};
  AudioTags::Parser parser;
  parser.begin((uint32_t)data.size(), readMemory, &memory);
  queueSources(parser, data, wav);
  unsigned count = 0;
  while (!parser.step(budget)) {
    if (++count > 100000) break;
  }
  if (steps) *steps = count + 1;
  return parser.info();
}

bool coverIsPng(const std::vector<uint8_t> &data, const AudioTags::Info &info) {
  static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  if (!info.hasCover() || info.coverLength < 20 || info.coverOffset > data.size() ||
      info.coverLength > data.size() - info.coverOffset) {
    return false;
  }
  const uint8_t *cover = data.data() + info.coverOffset;
  return memcmp(cover, signature, 8) == 0 && memcmp(cover + info.coverLength - 8, "IEND", 4) == 0;
}

void testDecodeText() {
  char out[AudioTags::FieldBytes];
  const uint8_t utf16Pair[] = {0xff, 0xfe, 0x3d, 0xd8, 0xb5, 0xdc, 'A', 0};  // U+1F4B5 then A
  AudioTags::decodeText(utf16Pair, sizeof(utf16Pair), 1, out, sizeof(out));
  checkText(out, "\xF0\x9F\x92\xB5" "A", "UTF-16LE surrogate pair");

  const uint8_t utf16Be[] = {0xfe, 0xff, 0x04, 0x1a, 0x00, 0x20, 0x00, 0x00, 0x00, 'x'};
  AudioTags::decodeText(utf16Be, sizeof(utf16Be), 1, out, sizeof(out));
  checkText(out, "\xD0\x9A", "UTF-16 BOM big-endian stops at the terminator and trims");

  const uint8_t lone[] = {0x00, 0xdc, 'b', 0};
  AudioTags::decodeText(lone, sizeof(lone), 1, out, sizeof(out));
  checkText(out, "?b", "an unpaired surrogate becomes ?");

  const uint8_t badUtf8[] = {'a', 0xc3, 'b', 0xe2, 0x82};
  AudioTags::decodeText(badUtf8, sizeof(badUtf8), 3, out, sizeof(out));
  checkText(out, "a?b??", "invalid UTF-8 bytes become ?");

  const uint8_t overlong[] = {0xc0, 0xaf, 'z'};
  AudioTags::decodeText(overlong, sizeof(overlong), 3, out, sizeof(out));
  checkText(out, "??z", "overlong UTF-8 is rejected");

  const uint8_t controls[] = {' ', ' ', 'a', '\t', 'b', '\n', ' ', ' '};
  AudioTags::decodeText(controls, sizeof(controls), 0, out, sizeof(out));
  checkText(out, "a b", "controls become spaces and ends are trimmed");

  const uint8_t latinUtf8[] = {'C', 'a', 'f', 0xc3, 0xa9};
  AudioTags::decodeText(latinUtf8, sizeof(latinUtf8), 0, out, sizeof(out));
  checkText(out, "Caf\xC3\xA9", "encoding 0 holding valid UTF-8 is read as UTF-8");

  const uint8_t latin1[] = {'C', 'a', 'f', 0xe9};
  AudioTags::decodeText(latin1, sizeof(latin1), 0, out, sizeof(out));
  checkText(out, "Caf\xC3\xA9", "isolated Latin-1 accent stays Latin-1");

  const uint8_t cp1251[] = {0xca, 0xe8, 0xed, 0xee, ' ', 0xa8};
  AudioTags::decodeText(cp1251, sizeof(cp1251), 0, out, sizeof(out));
  checkText(out, "\xD0\x9A\xD0\xB8\xD0\xBD\xD0\xBE \xD0\x81", "Windows-1251 run decodes as Cyrillic");

  const uint8_t mixed[] = {0xca, 0xe8, 0xed, 0x99};
  AudioTags::decodeText(mixed, sizeof(mixed), 0, out, sizeof(out));
  check(strncmp(out, "\xC3\x8A", 2) == 0, "a byte outside the Cyrillic range keeps Latin-1");

  // 100 two-byte characters do not fit 127 bytes: cut on a character boundary.
  std::vector<uint8_t> longCyrillic;
  for (int i = 0; i < 100; i++) { longCyrillic.push_back(0xd0); longCyrillic.push_back(0x96); }
  size_t length = AudioTags::decodeText(longCyrillic.data(), longCyrillic.size(), 3, out, sizeof(out));
  check(length == 126 && strlen(out) == 126, "long UTF-8 truncates to a whole character (126 bytes)");

  char tiny[4];
  const uint8_t word[] = {'a', 'b', 'c', 'd', 'e'};
  AudioTags::decodeText(word, sizeof(word), 0, tiny, sizeof(tiny));
  checkText(tiny, "abc", "output is bounded by its buffer");
}

void testFixtures() {
  {
    std::vector<uint8_t> data = load("v23_basic.mp3");
    AudioTags::Info info = parse(data, false);
    checkText(info.title, "Blue Monday", "v2.3 Latin-1 title");
    checkText(info.artist, "\xD0\x9A\xD0\xB8\xD0\xBD\xD0\xBE", "v2.3 UTF-16 artist");
    checkText(info.album, "Beyonc\xC3\xA9 Live", "v2.3 Latin-1 album keeps its accent");
    check(info.year == 1983, "v2.3 TYER year");
    check(info.coverType == 3 && coverIsPng(data, info), "v2.3 prefers the front cover listed second");
  }
  {
    std::vector<uint8_t> data = load("v24_utf8.mp3");
    AudioTags::Info info = parse(data, false);
    checkText(info.title, "\xC3\xBF" "es, \xC3\x9Cn\xC3\xAF" "code",
              "v2.4 unsynchronised UTF-16 title behind a data-length indicator");
    checkText(info.artist, "Lead Artist", "v2.4 TPE1 outranks an earlier TPE2");
    checkText(info.album, "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xD0\xBC\xD0\xB8\xD1\x80",
              "v2.4 UTF-8 album");
    check(info.year == 2004, "v2.4 TDRC date gives the year");
    check(coverIsPng(data, info), "v2.4 picture after an extended header");
  }
  {
    std::vector<uint8_t> data = load("v22.mp3");
    AudioTags::Info info = parse(data, false);
    checkText(info.title, "Old Format", "v2.2 TT2");
    checkText(info.artist, "Legacy Band", "v2.2 UTF-16BE TP1");
    checkText(info.album, "Twenty Two", "v2.2 TAL");
    check(info.year == 1999, "v2.2 TYE");
    check(coverIsPng(data, info), "v2.2 PIC");
  }
  {
    std::vector<uint8_t> data = load("v23_unsync.mp3");
    AudioTags::Info info = parse(data, false);
    checkText(info.title, "\xC3\xBF\xC3\xBF Sync", "tag-level unsynchronised UTF-16 title");
    checkText(info.artist, "Unsync Artist", "frames after unsynchronised data stay aligned");
    check(!info.hasCover(), "an unsynchronised picture is not offered");
  }
  {
    std::vector<uint8_t> data = load("v1_cp1251.mp3");
    AudioTags::Info info = parse(data, false);
    checkText(info.title, "\xD0\x93\xD1\x80\xD1\x83\xD0\xBF\xD0\xBF\xD0\xB0 "
                          "\xD0\xBA\xD1\x80\xD0\xBE\xD0\xB2\xD0\xB8",
              "ID3v1 Windows-1251 title");
    checkText(info.artist, "\xD0\x9A\xD0\xB8\xD0\xBD\xD0\xBE", "ID3v1 Windows-1251 artist");
    checkText(info.album, "Caf\xC3\xA9", "ID3v1 Latin-1 album");
    check(info.year == 1988 && !info.hasCover(), "ID3v1 year, no cover");
  }
  {
    std::vector<uint8_t> data = load("v2_plus_v1.mp3");
    AudioTags::Info info = parse(data, false);
    checkText(info.title, "From V2", "ID3v2 title outranks ID3v1");
    checkText(info.artist, "V1 Artist", "ID3v1 fills the missing artist");
    check(info.year == 2010, "ID3v1 fills the missing year");
  }
  {
    std::vector<uint8_t> data = load("untagged.mp3");
    AudioTags::Info info = parse(data, false);
    check(!info.title[0] && !info.artist[0] && !info.album[0] && !info.year && !info.hasCover(),
          "untagged MP3 yields nothing");
  }
  {
    std::vector<uint8_t> data = load("malformed.mp3");
    AudioTags::Info info = parse(data, false);
    checkText(info.title, "Kept Title", "an oversized frame ends the tag but keeps earlier text");
    check(!info.artist[0], "the oversized frame is not read");
  }
  {
    std::vector<uint8_t> data = load("wav_info.wav");
    AudioTags::Info info = parse(data, true);
    checkText(info.title, "Wave Title", "WAV INFO INAM");
    checkText(info.artist, "Wave Artist", "WAV INFO IART");
    checkText(info.album, "Wave Album", "WAV INFO IPRD");
    check(info.year == 2015, "WAV INFO ICRD");
  }
  {
    std::vector<uint8_t> data = load("wav_id3.wav");
    AudioTags::Info info = parse(data, true);
    checkText(info.title, "ID3 Title", "WAV id3 chunk outranks INFO");
    checkText(info.artist, "Info Artist", "WAV INFO fills what the id3 chunk lacks");
    check(coverIsPng(data, info), "WAV id3 chunk picture");
  }
}

void testIncremental() {
  const char *names[] = {"v23_basic.mp3", "v24_utf8.mp3", "wav_id3.wav"};
  for (const char *name : names) {
    std::vector<uint8_t> data = load(name);
    const bool wav = strstr(name, ".wav") != nullptr;
    unsigned steps = 0;
    AudioTags::Info whole = parse(data, wav);
    AudioTags::Info stepped = parse(data, wav, 1, &steps);
    check(memcmp(&whole, &stepped, sizeof(whole)) == 0 && steps > 3,
          std::string(name) + " parsed one frame per step matches a single pass");
  }
  // A large picture is skipped by offset, not read.
  std::vector<uint8_t> data = load("v23_basic.mp3");
  Memory memory{&data, 0};
  AudioTags::Parser parser;
  parser.begin((uint32_t)data.size(), readMemory, &memory);
  parser.addId3v2(0, (uint32_t)data.size());
  parser.step(1000);
  check(memory.reads < 20, "v2.3 tag is read with a handful of small reads");
}

bool infoIsSane(const AudioTags::Info &info, size_t fileSize) {
  for (const char *field : {info.title, info.artist, info.album}) {
    if (strnlen(field, AudioTags::FieldBytes) >= AudioTags::FieldBytes) return false;
  }
  if (info.year && (info.year < 1000 || info.year > 2999)) return false;
  if (info.hasCover() && (info.coverOffset > fileSize || info.coverLength > fileSize - info.coverOffset)) {
    return false;
  }
  return true;
}

void testRobustness() {
  const char *names[] = {"v23_basic.mp3", "v24_utf8.mp3", "v22.mp3", "v23_unsync.mp3",
                         "v1_cp1251.mp3", "wav_id3.wav", "wav_info.wav", "malformed.mp3"};
  bool sane = true;
  for (const char *name : names) {
    const std::vector<uint8_t> original = load(name);
    const bool wav = strstr(name, ".wav") != nullptr;
    // Every truncation of the tag region and the ID3v1 tail.
    const size_t limit = original.size() < 2048 ? original.size() : 2048;
    for (size_t length = 0; length <= limit; length++) {
      std::vector<uint8_t> cut(original.begin(), original.begin() + length);
      sane = sane && infoIsSane(parse(cut, wav), cut.size());
    }
    // Deterministic byte corruption in the first 1 KiB.
    uint32_t seed = 0x1234567u;
    for (int round = 0; round < 3000; round++) {
      std::vector<uint8_t> mutated = original;
      for (int k = 0; k < 4; k++) {
        seed = seed * 1103515245u + 12345u;
        size_t position = (seed >> 8) % (mutated.size() < 1024 ? mutated.size() : 1024);
        seed = seed * 1103515245u + 12345u;
        mutated[position] = (uint8_t)(seed >> 16);
      }
      sane = sane && infoIsSane(parse(mutated, wav), mutated.size());
    }
  }
  check(sane, "truncated and corrupted tags stay bounded");
}
}  // namespace

int main(int argc, char **argv) {
  fixtureDir = argc > 1 ? argv[1] : "tests/audio_tags/fixtures";
  testDecodeText();
  testFixtures();
  testIncremental();
  testRobustness();
  printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
