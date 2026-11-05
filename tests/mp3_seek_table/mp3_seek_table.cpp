// Host contract test for Mp3SeekTable: interval doubling, lookup with one
// warm-up frame, and validation of a stored (.empa v2) table.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../../src/media/Mp3SeekTable.h"

namespace {
int failures = 0;

void check(bool condition, const char *name) {
  std::printf("%s: %s\n", condition ? "ok" : "FAIL", name);
  if (!condition) failures++;
}

// Frame i starts at dataOffset + i * 417, like 128 kbps MPEG-1 Layer III.
constexpr uint32_t DataOffset = 1000;
constexpr uint32_t FrameBytes = 417;

uint32_t offsetOf(uint32_t frame) { return DataOffset + frame * FrameBytes; }

void build(Mp3SeekTable &table, uint32_t frames) {
  table.clear();
  for (uint32_t i = 0; i < frames; i++) table.addFrame(i, offsetOf(i));
}

bool entriesMatch(const Mp3SeekTable &table) {
  for (uint16_t i = 0; i < table.count(); i++) {
    if (table.offsets()[i] != offsetOf(i * table.interval())) return false;
  }
  return true;
}
}  // namespace

int main() {
  std::vector<uint32_t> storage(Mp3SeekTable::MaxEntries);
  Mp3SeekTable table;
  table.attach(storage.data(), Mp3SeekTable::MaxEntries);

  build(table, 1);
  check(table.count() == 1 && table.interval() == 64 && table.offsets()[0] == DataOffset,
        "a one-frame stream has one entry at the data offset");

  build(table, 64 * 4096);
  check(table.count() == 4096 && table.interval() == 64 && entriesMatch(table),
        "64 x 4096 frames fill the table at the initial interval");

  build(table, 64 * 4096 + 1);
  check(table.count() == 2049 && table.interval() == 128 && entriesMatch(table),
        "one more frame doubles the interval and halves the entries");

  build(table, 10'000'000);
  check(table.complete() && table.count() <= 4096 && entriesMatch(table) &&
          table.count() == (10'000'000 + table.interval() - 1) / table.interval(),
        "a very long stream stays within capacity with one entry per interval");

  build(table, 1000);
  uint32_t entryFrame = 0, offset = 0;
  check(table.lookup(0, entryFrame, offset) && entryFrame == 0 && offset == DataOffset,
        "frame 0 restarts at the first entry");
  check(table.lookup(64, entryFrame, offset) && entryFrame == 0,
        "an entry's own frame restarts one entry earlier, for the warm-up frame");
  check(table.lookup(65, entryFrame, offset) && entryFrame == 64 && offset == offsetOf(64),
        "the frame after an entry restarts at that entry");
  check(table.lookup(999, entryFrame, offset) && entryFrame == 960,
        "the last frame restarts at the last entry");
  check(table.lookup(5000, entryFrame, offset) && entryFrame == 960,
        "a frame past the end clamps to the last entry");

  const uint32_t frames = 1000;
  const uint32_t audioEnd = offsetOf(frames);
  std::vector<uint32_t> stored(table.offsets(), table.offsets() + table.count());
  check(Mp3SeekTable::valid(64, stored.size(), stored.data(), DataOffset, audioEnd, frames),
        "a built table validates");
  check(!Mp3SeekTable::valid(64, stored.size() - 1, stored.data(), DataOffset, audioEnd, frames),
        "a missing entry is rejected");
  check(!Mp3SeekTable::valid(96, stored.size(), stored.data(), DataOffset, audioEnd, frames),
        "a non-power-of-two interval is rejected");
  check(!Mp3SeekTable::valid(32, stored.size(), stored.data(), DataOffset, audioEnd, frames),
        "an interval below the initial one is rejected");
  check(!Mp3SeekTable::valid(64, stored.size(), stored.data(), DataOffset + 1, audioEnd, frames),
        "a first entry away from the data offset is rejected");
  check(!Mp3SeekTable::valid(64, stored.size(), stored.data(), DataOffset, stored.back(), frames),
        "an entry at or past the audio end is rejected");
  std::vector<uint32_t> unordered = stored;
  std::swap(unordered[3], unordered[4]);
  check(!Mp3SeekTable::valid(64, unordered.size(), unordered.data(), DataOffset, audioEnd, frames),
        "entries out of order are rejected");
  std::vector<uint32_t> repeated = stored;
  repeated[5] = repeated[4];
  check(!Mp3SeekTable::valid(64, repeated.size(), repeated.data(), DataOffset, audioEnd, frames),
        "a repeated entry is rejected");

  std::copy(stored.begin(), stored.end(), storage.begin());
  check(table.adopt(64, stored.size(), DataOffset, audioEnd, frames) && table.count() == stored.size(),
        "a valid stored table is adopted");
  check(!table.adopt(64, stored.size(), DataOffset + 1, audioEnd, frames) && table.count() == 0,
        "an invalid stored table leaves the table empty");

  Mp3SeekTable detached;
  detached.addFrame(0, DataOffset);
  check(detached.count() == 0 && !detached.lookup(0, entryFrame, offset),
        "a table without storage records and finds nothing");

  if (failures) {
    std::printf("mp3_seek_table: %d failed\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("mp3_seek_table: all checks passed\n");
  return EXIT_SUCCESS;
}
