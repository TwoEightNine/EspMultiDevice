#pragma once

#include <Arduino.h>

// Project-owned FatFs block driver for the shared-SPI SD card.
//
// Arduino-ESP32's SD backend still powers up, initializes, and mounts the
// card. attach() then replaces that backend's FatFs callbacks for the same
// drive. Compared with the backend it:
// - switches the card to High-Speed (CMD6) and runs the bus at 40 or
//   26.67 MHz instead of the backend's effective 20 MHz, keeping data CRC on
//   so a marginal clock is detected and stepped down instead of corrupting;
// - moves each 512-byte block and its CRC with one GDMA transaction into
//   double-buffered internal RAM, copying and checking the previous block
//   while the next one is on the wire. DMA is verified against a FIFO read
//   at attach; a GDMA fault is retried, and without DMA, or after repeated
//   faults, blocks stream through
//   the 64-byte SPI FIFO with the CRC16 overlapped with the next chunk;
// - answers FatFs status polls from cached state instead of issuing CMD13 on
//   every f_read/f_write/f_lseek.
//
// The driver drives the GPSPI2 registers behind the global SPIClass `SPI`
// inside its SPI transactions, so it requires the card on that bus. All entry
// points must be called with SpiBusLock held or from FatFs calls made under it.
namespace SdSpiDriver {

struct Info {
  bool attached;
  bool highSpeed;
  bool crc;
  bool dma;
  // GDMA faults since the last attach, and the most recent one with the
  // descriptor, GDMA RX channel, and SPI DMA register state (empty if none).
  uint8_t dmaFaults;
  const char *dmaFault;
  uint32_t clockHz;
  uint32_t retries;
  uint32_t stepDowns;
};

// Diagnostic timing since the last resetStats(). Times are wall-clock
// microseconds with the bus held; "access" is the wait for each read
// command's first data token and "gap" the wait for later tokens.
struct Stats {
  uint32_t readCalls;
  uint32_t readSectors;
  uint32_t maxReadSectors;
  uint32_t writeCalls;
  uint32_t writeSectors;
  uint32_t maxWriteSectors;
  uint32_t readUs;
  uint32_t accessUs;
  uint32_t gapUs;
  uint32_t dataUs;
  uint32_t stopUs;
  uint32_t writeUs;
  uint32_t writeBusyUs;
};

// blockAddressing is true for SDHC/SDXC (sector-addressed) cards.
bool attach(uint8_t pdrv, uint8_t csPin, bool blockAddressing, uint32_t sectors);
// Stops using the drive. The caller then ends or unregisters the backend.
void detach();
bool readBlocks(uint32_t sector, uint8_t *buffer, uint32_t count);
bool writeBlocks(uint32_t sector, const uint8_t *buffer, uint32_t count);
Info info();
Stats stats();
void resetStats();

}
