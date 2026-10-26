#include "SdSpiDriver.h"

#include <SPI.h>
#include <esp32-hal-psram.h>
#include <esp_cpu.h>
#include <soc/spi_struct.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include <ff.h>
#include <diskio_impl.h>
}

namespace {

constexpr uint32_t BlockBytes = 512;
constexpr uint32_t FifoBytes = 64;
constexpr uint8_t SwitchFunction = 6;
constexpr uint8_t StopTransmission = 12;
constexpr uint8_t SendStatus = 13;
constexpr uint8_t ReadSingle = 17;
constexpr uint8_t ReadMultiple = 18;
constexpr uint8_t SetWriteEraseCount = 23;
constexpr uint8_t WriteSingle = 24;
constexpr uint8_t WriteMultiple = 25;
constexpr uint8_t AppCommand = 55;
constexpr uint8_t CrcOnOff = 59;
constexpr uint8_t DataStartToken = 0xFE;
constexpr uint8_t MultipleWriteToken = 0xFC;
constexpr uint8_t StopTranToken = 0xFD;
constexpr uint32_t SelectTimeoutMs = 300;
constexpr uint32_t ReadTokenTimeoutMs = 250;
constexpr uint32_t BusyTimeoutMs = 600;
constexpr uint8_t MaxAttempts = 4;
constexpr uint8_t FailuresBeforeStepDown = 2;
// Probe the selected clock with this many multi-block reads of the first
// sectors, compared with a reference read at the backend's 20 MHz rate.
constexpr uint32_t ProbeSectors = 64;
constexpr uint8_t ProbePasses = 3;
constexpr uint8_t ClockLevels = 3;

spi_dev_t &bus = GPSPI2;

struct State {
  bool attached;
  bool highSpeed;
  bool crc;
  bool blockAddressing;
  uint8_t pdrv;
  uint8_t csPin;
  uint8_t level;
  uint8_t fastestLevel;
  uint32_t sectors;
  uint32_t clocks[ClockLevels];
  uint32_t retries;
  uint32_t stepDowns;
};

State state = {};

// CPU cycle accumulators behind SdSpiDriver::Stats. Each measured interval
// is far below the 32-bit counter's wrap period.
struct Cycles {
  uint32_t readCalls, readSectors, maxReadSectors;
  uint32_t writeCalls, writeSectors, maxWriteSectors;
  uint64_t read, access, gap, data, stop, write, writeBusy;
};

Cycles cycles = {};

inline uint32_t now() {
  return esp_cpu_get_cycle_count();
}

uint32_t toMicros(uint64_t value) {
  return static_cast<uint32_t>(value / getCpuFrequencyMhz());
}

struct Crc16Table {
  uint16_t values[256];
  constexpr Crc16Table() : values() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint16_t crc = static_cast<uint16_t>(i << 8);
      for (uint8_t bit = 0; bit < 8; ++bit) {
        crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                             : static_cast<uint16_t>(crc << 1);
      }
      values[i] = crc;
    }
  }
};

DRAM_ATTR constexpr Crc16Table Crc16 = Crc16Table();

inline uint16_t updateCrc16(uint16_t crc, const uint8_t *data, uint32_t length) {
  for (uint32_t i = 0; i < length; ++i) {
    crc = static_cast<uint16_t>((crc << 8) ^ Crc16.values[((crc >> 8) ^ data[i]) & 0xFF]);
  }
  return crc;
}

uint8_t crc7(const uint8_t *data, uint32_t length) {
  uint8_t crc = 0;
  for (uint32_t i = 0; i < length; ++i) {
    uint8_t value = data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc <<= 1;
      if ((value ^ crc) & 0x80) crc ^= 0x09;
      value <<= 1;
    }
  }
  return crc & 0x7F;
}

// Same CPU-controlled full-duplex register sequence as the Arduino HAL's NL
// transfers. The FIFO holds 16 words; bytes leave from the low byte first.
inline void startTransfer(uint32_t bytes) {
  bus.ms_dlen.ms_data_bitlen = bytes * 8 - 1;
  bus.cmd.update = 1;
  while (bus.cmd.update) {}
  bus.cmd.usr = 1;
}

inline void waitTransfer() {
  while (bus.cmd.usr) {}
}

inline void fillOnes(uint32_t bytes) {
  const uint32_t words = (bytes + 3) / 4;
  for (uint32_t i = 0; i < words; ++i) bus.data_buf[i] = 0xFFFFFFFFUL;
}

uint8_t transferByte(uint8_t value) {
  bus.data_buf[0] = value;
  startTransfer(1);
  waitTransfer();
  return static_cast<uint8_t>(bus.data_buf[0] & 0xFF);
}

uint16_t receiveWord() {
  bus.data_buf[0] = 0xFFFFFFFFUL;
  startTransfer(2);
  waitTransfer();
  const uint32_t value = bus.data_buf[0];
  return static_cast<uint16_t>(((value & 0xFF) << 8) | ((value >> 8) & 0xFF));
}

bool waitReady(uint32_t timeoutMs) {
  const uint32_t started = millis();
  do {
    if (transferByte(0xFF) == 0xFF) return true;
  } while (millis() - started < timeoutMs);
  return false;
}

void deselect() {
  digitalWrite(state.csPin, HIGH);
  // Eight clocks with CS high make the card release MISO, which the touch
  // controller shares.
  transferByte(0xFF);
}

bool select() {
  digitalWrite(state.csPin, LOW);
  if (waitReady(SelectTimeoutMs)) return true;
  deselect();
  return false;
}

uint8_t command(uint8_t index, uint32_t argument) {
  uint8_t packet[6] = {
    static_cast<uint8_t>(0x40 | index),
    static_cast<uint8_t>(argument >> 24),
    static_cast<uint8_t>(argument >> 16),
    static_cast<uint8_t>(argument >> 8),
    static_cast<uint8_t>(argument),
    0
  };
  packet[5] = static_cast<uint8_t>((crc7(packet, 5) << 1) | 0x01);
  bus.data_buf[0] = packet[0] | (packet[1] << 8) | (packet[2] << 16) |
                    (static_cast<uint32_t>(packet[3]) << 24);
  bus.data_buf[1] = packet[4] | (packet[5] << 8);
  startTransfer(sizeof(packet));
  waitTransfer();
  // CMD12 is followed by a stuff byte that may still carry read data.
  if (index == StopTransmission) transferByte(0xFF);
  for (uint8_t i = 0; i < 10; ++i) {
    const uint8_t response = transferByte(0xFF);
    if (!(response & 0x80)) return response;
  }
  return 0xFF;
}

uint32_t address(uint32_t sector) {
  return state.blockAddressing ? sector : sector * BlockBytes;
}

// Receives one data block. The next FIFO chunk is clocked while the CPU
// copies and checksums the previous one.
bool receiveBlock(uint8_t *destination, uint32_t length, uint64_t *tokenCycles = nullptr) {
  const uint32_t started = millis();
  const uint32_t tokenStarted = now();
  uint8_t token;
  do {
    token = transferByte(0xFF);
  } while (token == 0xFF && millis() - started < ReadTokenTimeoutMs);
  const uint32_t dataStarted = now();
  if (tokenCycles) *tokenCycles += dataStarted - tokenStarted;
  if (token != DataStartToken) return false;

  const bool aligned = (reinterpret_cast<uintptr_t>(destination) & 3) == 0;
  uint32_t bounce[FifoBytes / 4];
  uint16_t crc = 0;
  uint32_t done = 0;
  uint32_t chunk = length < FifoBytes ? length : FifoBytes;
  fillOnes(chunk);
  startTransfer(chunk);
  while (done < length) {
    waitTransfer();
    const uint32_t received = chunk;
    uint32_t *words = aligned ? reinterpret_cast<uint32_t *>(destination + done) : bounce;
    for (uint32_t i = 0; i < received / 4; ++i) words[i] = bus.data_buf[i];
    const uint32_t next = done + received;
    if (next < length) {
      chunk = length - next < FifoBytes ? length - next : FifoBytes;
      fillOnes(chunk);
      startTransfer(chunk);
    }
    if (!aligned) memcpy(destination + done, bounce, received);
    if (state.crc) crc = updateCrc16(crc, destination + done, received);
    done = next;
  }
  const uint16_t expected = receiveWord();
  cycles.data += now() - dataStarted;
  return !state.crc || expected == crc;
}

bool sendBlock(uint8_t token, const uint8_t *source) {
  transferByte(token);
  const bool aligned = (reinterpret_cast<uintptr_t>(source) & 3) == 0;
  uint32_t bounce[FifoBytes / 4];
  uint16_t crc = 0;
  for (uint32_t offset = 0; offset < BlockBytes; offset += FifoBytes) {
    const uint8_t *chunk = source + offset;
    const uint32_t *words = reinterpret_cast<const uint32_t *>(chunk);
    if (!aligned) {
      memcpy(bounce, chunk, FifoBytes);
      words = bounce;
    }
    if (offset) waitTransfer();
    for (uint32_t i = 0; i < FifoBytes / 4; ++i) bus.data_buf[i] = words[i];
    startTransfer(FifoBytes);
    if (state.crc) crc = updateCrc16(crc, chunk, FifoBytes);
  }
  waitTransfer();
  if (!state.crc) crc = 0xFFFF;
  bus.data_buf[0] = (crc >> 8) | ((crc & 0xFF) << 8);
  startTransfer(2);
  waitTransfer();
  return (transferByte(0xFF) & 0x1F) == 0x05;
}

bool readStatus() {
  if (!select()) return false;
  const uint8_t r1 = command(SendStatus, 0);
  const uint8_t r2 = transferByte(0xFF);
  deselect();
  return r1 == 0 && r2 == 0;
}

bool readOnce(uint32_t sector, uint8_t *buffer, uint32_t count) {
  if (!select()) return false;
  const bool multiple = count > 1;
  const bool started = command(multiple ? ReadMultiple : ReadSingle, address(sector)) == 0;
  bool ok = started;
  for (uint32_t i = 0; ok && i < count; ++i) {
    ok = receiveBlock(buffer + i * BlockBytes, BlockBytes, i ? &cycles.gap : &cycles.access);
  }
  if (started && multiple) {
    const uint32_t stopStarted = now();
    // Data integrity is already proven by each block CRC. The CMD12 R1 can
    // report harmless out-of-range bits after reading the last blocks.
    command(StopTransmission, 0);
    ok = waitReady(BusyTimeoutMs) && ok;
    cycles.stop += now() - stopStarted;
  }
  deselect();
  return ok;
}

bool waitWriteReady() {
  const uint32_t started = now();
  const bool ready = waitReady(BusyTimeoutMs);
  cycles.writeBusy += now() - started;
  return ready;
}

bool writeOnce(uint32_t sector, const uint8_t *buffer, uint32_t count) {
  if (!select()) return false;
  bool ok;
  if (count == 1) {
    ok = command(WriteSingle, address(sector)) == 0 &&
         sendBlock(DataStartToken, buffer) && waitWriteReady();
  } else {
    // Optional pre-erase hint; cards that reject it still accept CMD25.
    if (command(AppCommand, 0) <= 1) command(SetWriteEraseCount, count);
    const bool started = command(WriteMultiple, address(sector)) == 0;
    ok = started;
    for (uint32_t i = 0; ok && i < count; ++i) {
      ok = sendBlock(MultipleWriteToken, buffer + i * BlockBytes) && waitWriteReady();
    }
    if (started) {
      transferByte(StopTranToken);
      transferByte(0xFF);
      ok = waitWriteReady() && ok;
    }
  }
  deselect();
  return ok && readStatus();
}

void beginBus() {
  SPI.beginTransaction(SPISettings(state.clocks[state.level], MSBFIRST, SPI_MODE0));
}

void endBus() {
  SPI.endTransaction();
}

bool stepDown() {
  if (state.level + 1 >= ClockLevels) return false;
  const uint32_t from = state.clocks[state.level];
  ++state.level;
  ++state.stepDowns;
  Serial.printf("SD SPI errors at %.2f MHz; continuing at %.2f MHz.\n",
                from / 1000000.0f, state.clocks[state.level] / 1000000.0f);
  return true;
}

template <typename Operation>
bool withRetries(Operation operation) {
  uint8_t failuresAtLevel = 0;
  for (uint8_t attempt = 0; attempt < MaxAttempts; ++attempt) {
    beginBus();
    const bool ok = operation();
    endBus();
    if (ok) return true;
    ++state.retries;
    if (++failuresAtLevel >= FailuresBeforeStepDown && stepDown()) failuresAtLevel = 0;
  }
  return false;
}

bool switchFunction(uint32_t argument, uint8_t *status) {
  if (!select()) return false;
  const bool ok = command(SwitchFunction, argument) == 0 && receiveBlock(status, 64);
  deselect();
  return ok;
}

// CMD6 High-Speed switch, valid in SPI mode for SD 1.10+ cards. Function
// group 1 support is status bits 415:400 and the selection result bits
// 379:376 (status bytes 13 and 16, most significant bit first).
bool enableHighSpeed() {
  alignas(4) uint8_t status[64];
  if (!switchFunction(0x00FFFFF1UL, status) || !(status[13] & 0x02)) return false;
  if (!switchFunction(0x80FFFFF1UL, status)) return false;
  return (status[16] & 0x0F) == 1;
}

bool enableCrc() {
  if (!select()) return false;
  const uint8_t response = command(CrcOnOff, 1);
  deselect();
  return response == 0;
}

bool probeLevel(uint8_t level, const uint8_t *reference, uint8_t *check) {
  state.level = level;
  for (uint8_t pass = 0; pass < ProbePasses; ++pass) {
    beginBus();
    const bool ok = readOnce(0, check, ProbeSectors);
    endBus();
    if (!ok || memcmp(reference, check, ProbeSectors * BlockBytes) != 0) return false;
  }
  return true;
}

uint8_t selectFastestLevel() {
  const uint8_t slowest = ClockLevels - 1;
  if (state.fastestLevel == slowest) return slowest;
  const size_t bytes = ProbeSectors * BlockBytes;
  uint8_t *reference = static_cast<uint8_t *>(ps_malloc(bytes));
  uint8_t *check = static_cast<uint8_t *>(ps_malloc(bytes));
  uint8_t chosen = slowest;
  if (reference && check) {
    state.level = slowest;
    beginBus();
    const bool haveReference = readOnce(0, reference, ProbeSectors);
    endBus();
    for (uint8_t level = state.fastestLevel; haveReference && level < slowest; ++level) {
      if (probeLevel(level, reference, check)) {
        chosen = level;
        break;
      }
      Serial.printf("SD SPI probe failed at %.2f MHz.\n", state.clocks[level] / 1000000.0f);
    }
  }
  free(reference);
  free(check);
  return chosen;
}

DSTATUS diskStatus(unsigned char pdrv) {
  return state.attached && pdrv == state.pdrv ? 0 : STA_NOINIT;
}

DRESULT diskRead(unsigned char pdrv, unsigned char *buffer, uint32_t sector, unsigned count) {
  if (diskStatus(pdrv)) return RES_NOTRDY;
  return SdSpiDriver::readBlocks(sector, buffer, count) ? RES_OK : RES_ERROR;
}

DRESULT diskWrite(unsigned char pdrv, const unsigned char *buffer, uint32_t sector, unsigned count) {
  if (diskStatus(pdrv)) return RES_NOTRDY;
  return SdSpiDriver::writeBlocks(sector, buffer, count) ? RES_OK : RES_ERROR;
}

DRESULT diskIoctl(unsigned char pdrv, unsigned char command, void *buffer) {
  if (diskStatus(pdrv)) return RES_NOTRDY;
  switch (command) {
    case CTRL_SYNC:
      return withRetries([] {
        if (!select()) return false;
        deselect();
        return true;
      }) ? RES_OK : RES_ERROR;
    case GET_SECTOR_COUNT: *static_cast<LBA_t *>(buffer) = state.sectors; return RES_OK;
    case GET_SECTOR_SIZE: *static_cast<WORD *>(buffer) = BlockBytes; return RES_OK;
    case GET_BLOCK_SIZE: *static_cast<DWORD *>(buffer) = 1; return RES_OK;
  }
  return RES_PARERR;
}

const ff_diskio_impl_t DiskIo = {
  .init = &diskStatus,
  .status = &diskStatus,
  .read = &diskRead,
  .write = &diskWrite,
  .ioctl = &diskIoctl
};

}

namespace SdSpiDriver {

bool attach(uint8_t pdrv, uint8_t csPin, bool blockAddressing, uint32_t sectors) {
  detach();
  if (pdrv >= FF_VOLUMES || sectors < ProbeSectors) return false;
  state.pdrv = pdrv;
  state.csPin = csPin;
  state.blockAddressing = blockAddressing;
  state.sectors = sectors;
  state.retries = state.stepDowns = 0;
  const uint32_t apb = getApbFrequency();
  state.clocks[0] = apb / 2;
  state.clocks[1] = apb / 3;
  state.clocks[2] = apb / 4;

  // Card setup runs at the backend's proven 20 MHz rate. A High-Speed card
  // tolerates any lower clock, so the backend's own raw-sector health check
  // keeps working after the switch.
  state.level = ClockLevels - 1;
  state.crc = false;
  beginBus();
  state.crc = enableCrc();
  state.highSpeed = state.crc && enableHighSpeed();
  endBus();
  // Never exceed the 25 MHz default-speed limit without High-Speed mode, and
  // never run faster than the base clock without CRC to detect errors.
  state.fastestLevel = state.highSpeed ? 0 : ClockLevels - 1;
  state.level = selectFastestLevel();

  state.attached = true;
  ff_diskio_register(pdrv, &DiskIo);
  Serial.printf("SD SPI driver: %.2f MHz, High-Speed %s, CRC %s.\n",
                state.clocks[state.level] / 1000000.0f,
                state.highSpeed ? "on" : "unavailable", state.crc ? "on" : "off");
  return true;
}

void detach() {
  state.attached = false;
}

bool readBlocks(uint32_t sector, uint8_t *buffer, uint32_t count) {
  if (!state.attached || !buffer || !count || sector >= state.sectors ||
      count > state.sectors - sector) return false;
  const uint32_t started = now();
  const bool ok = withRetries([=] { return readOnce(sector, buffer, count); });
  cycles.read += now() - started;
  ++cycles.readCalls;
  cycles.readSectors += count;
  if (count > cycles.maxReadSectors) cycles.maxReadSectors = count;
  return ok;
}

bool writeBlocks(uint32_t sector, const uint8_t *buffer, uint32_t count) {
  if (!state.attached || !buffer || !count || sector >= state.sectors ||
      count > state.sectors - sector) return false;
  const uint32_t started = now();
  const bool ok = withRetries([=] { return writeOnce(sector, buffer, count); });
  cycles.write += now() - started;
  ++cycles.writeCalls;
  cycles.writeSectors += count;
  if (count > cycles.maxWriteSectors) cycles.maxWriteSectors = count;
  return ok;
}

Info info() {
  return {state.attached, state.highSpeed, state.crc,
          state.attached ? state.clocks[state.level] : 0, state.retries, state.stepDowns};
}

Stats stats() {
  return {cycles.readCalls, cycles.readSectors, cycles.maxReadSectors,
          cycles.writeCalls, cycles.writeSectors, cycles.maxWriteSectors,
          toMicros(cycles.read), toMicros(cycles.access), toMicros(cycles.gap),
          toMicros(cycles.data), toMicros(cycles.stop), toMicros(cycles.write),
          toMicros(cycles.writeBusy)};
}

void resetStats() {
  cycles = {};
}

}
