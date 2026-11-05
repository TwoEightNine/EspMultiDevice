#pragma once

#include <stddef.h>
#include <stdint.h>

// Arduino-free MP3 seek table: the byte offset of every Nth frame, built
// during the Audio Player's frame scan and stored in the `.empa` v2 cache.
// N starts at InitialInterval and doubles (dropping every other entry)
// whenever the table would pass its capacity, so any length fits in a fixed
// buffer the caller owns. tests/mp3_seek_table covers the contract on the host.
class Mp3SeekTable {
public:
  static constexpr uint32_t InitialInterval = 64;
  static constexpr uint32_t MaxInterval = 1UL << 20;
  static constexpr uint16_t MaxEntries = 4096;

  // `storage` holds `capacity` entries (2..MaxEntries) and outlives the table.
  void attach(uint32_t *storage, uint16_t capacity);
  void detach();
  void clear();
  bool attached() const { return _offsets != nullptr; }

  // Feed every frame of the stream in order, starting at index 0.
  void addFrame(uint32_t frameIndex, uint32_t offset);

  // False once the interval reached MaxInterval and later frames went
  // unindexed; such a table must not be stored. Unreachable for 32-bit
  // sample counts, but checked rather than assumed.
  bool complete() const { return !_full; }
  uint32_t interval() const { return _interval; }
  uint16_t count() const { return _count; }
  const uint32_t *offsets() const { return _offsets; }

  // Where to restart decoding to reach `frameIndex`: the last entry before it
  // (so at least one frame is decoded and discarded to refill the bit
  // reservoir), or the first entry for frame 0. False when the table is empty.
  bool lookup(uint32_t frameIndex, uint32_t &entryFrame, uint32_t &offset) const;

  // The rules a stored table must meet for a stream of `frameCount` MP3
  // frames between dataOffset and audioEnd: a power-of-two interval from
  // InitialInterval to MaxInterval, exactly one entry per interval, the first
  // at dataOffset, and the rest strictly increasing and below audioEnd.
  static bool valid(uint32_t interval, uint32_t count, const uint32_t *offsets,
                    uint32_t dataOffset, uint32_t audioEnd, uint32_t frameCount);
  // Adopts entries already copied into the attached storage after valid().
  bool adopt(uint32_t interval, uint16_t count, uint32_t dataOffset, uint32_t audioEnd,
             uint32_t frameCount);

private:
  uint32_t *_offsets = nullptr;
  uint16_t _capacity = 0;
  uint16_t _count = 0;
  uint32_t _interval = InitialInterval;
  // Set once the interval cannot double any more; later frames are not indexed.
  bool _full = false;
};
