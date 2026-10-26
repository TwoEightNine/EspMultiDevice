#include "SdSpiDriver.h"

#include <SPI.h>
#include <esp32-hal-psram.h>
#include <esp_cpu.h>
#include <esp_heap_caps.h>
#include <esp_private/gdma.h>
#include <esp_private/periph_ctrl.h>
#include <hal/dma_types.h>
#include <soc/gdma_struct.h>
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
// One DMA transaction moves a whole block plus its CRC (reads), or the start
// token, block, and CRC (writes). Buffers are word-sized for GDMA.
constexpr uint32_t BlockWithCrcBytes = BlockBytes + 2;
constexpr uint32_t TokenBlockWithCrcBytes = BlockWithCrcBytes + 1;
constexpr uint32_t DmaBufferBytes = (TokenBlockWithCrcBytes + 3) & ~3UL;
// A block takes about 200 us on the wire at 20 MHz; these only catch a
// GDMA that never completes, which permanently selects FIFO transfers.
constexpr uint32_t DmaTransferTimeoutUs = 5000;
constexpr uint32_t DmaWritebackTimeoutUs = 500;
// A fault resets both channels and the operation is retried; this many in
// one mount select FIFO transfers until the next attach.
constexpr uint8_t MaxDmaFaults = 3;
// Channel pairs abandoned because another driver reprogrammed them, before
// FIFO transfers are selected for the rest of the firmware's run.
constexpr uint8_t MaxPairMoves = 2;

spi_dev_t &bus = GPSPI2;

struct State {
  bool attached;
  bool highSpeed;
  bool crc;
  bool blockAddressing;
  bool dma;
  // Set by a DMA fault so the retry does not count it as a clock failure.
  bool dmaFaultPending;
  uint8_t dmaFaults;
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

// GDMA channel pair on SPI2 and its internal-RAM descriptors and buffers.
// Allocated on the first attach and kept for the life of the firmware.
struct Dma {
  bool ready;
  int pair;
  uint8_t moves;
  gdma_channel_handle_t tx;
  gdma_channel_handle_t rx;
  dma_descriptor_t *txDescriptor;
  dma_descriptor_t *rxDescriptors;  // one per receive slot
  uint8_t *ones;                    // 0xFF clocked out while receiving
  uint8_t *slots[2];                // double-buffered receive blocks
  uint8_t *outgoing;                // staged token, block, and CRC
};

Dma dma = {};
// The most recent DMA fault since the last attach; empty if none.
char dmaFault[192] = "";

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
bool waitDataToken(uint64_t *tokenCycles) {
  const uint32_t started = millis();
  const uint32_t tokenStarted = now();
  uint8_t token;
  do {
    token = transferByte(0xFF);
  } while (token == 0xFF && millis() - started < ReadTokenTimeoutMs);
  if (tokenCycles) *tokenCycles += now() - tokenStarted;
  return token == DataStartToken;
}

bool receiveBlock(uint8_t *destination, uint32_t length, uint64_t *tokenCycles = nullptr) {
  if (!waitDataToken(tokenCycles)) return false;
  const uint32_t dataStarted = now();

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

bool sendBlockFifo(uint8_t token, const uint8_t *source) {
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

void armDescriptor(dma_descriptor_t *descriptor, uint8_t *buffer, uint32_t length) {
  descriptor->dw0.size = DmaBufferBytes;
  descriptor->dw0.length = length;
  descriptor->dw0.err_eof = 0;
  descriptor->dw0.suc_eof = 1;
  descriptor->dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
  descriptor->buffer = buffer;
  descriptor->next = nullptr;
}

// Starts one full-duplex SPI transaction of `length` bytes, sent from
// `outgoing` by the TX channel and captured into `slot` by the RX channel.
// Writes discard what they receive, so reads and writes run the single
// configuration that the attach probe verifies. The HAL's full-duplex setup
// stays in place; only the data path changes.
void startDma(const uint8_t *outgoing, uint32_t length, uint8_t slot) {
  armDescriptor(dma.txDescriptor, const_cast<uint8_t *>(outgoing), length);
  armDescriptor(&dma.rxDescriptors[slot], dma.slots[slot], 0);
  dma.rxDescriptors[slot].dw0.suc_eof = 0;
  __sync_synchronize();
  gdma_reset(dma.tx);
  gdma_reset(dma.rx);
  bus.dma_conf.rx_afifo_rst = 1;
  bus.dma_conf.buf_afifo_rst = 1;
  bus.dma_conf.dma_afifo_rst = 1;
  bus.dma_conf.rx_afifo_rst = 0;
  bus.dma_conf.buf_afifo_rst = 0;
  bus.dma_conf.dma_afifo_rst = 0;
  bus.dma_int_clr.infifo_full_err = 1;
  bus.dma_int_clr.outfifo_empty_err = 1;
  bus.dma_conf.dma_rx_ena = 1;
  bus.dma_conf.dma_tx_ena = 1;
  gdma_start(dma.rx, reinterpret_cast<intptr_t>(&dma.rxDescriptors[slot]));
  gdma_start(dma.tx, reinterpret_cast<intptr_t>(dma.txDescriptor));
  startTransfer(length);
}

bool pairConnected() {
  return dma.tx && GDMA.channel[dma.pair].in.peri_sel.sel == SOC_GDMA_TRIG_PERIPH_SPI2 &&
         GDMA.channel[dma.pair].out.peri_sel.sel == SOC_GDMA_TRIG_PERIPH_SPI2;
}

void disableDma() {
  // Never stop a pair that another driver has reprogrammed and may be using.
  if (pairConnected()) {
    gdma_stop(dma.tx);
    gdma_stop(dma.rx);
  }
  state.dma = false;
}

bool acquirePair();

// Another driver has reconnected our pair to its own peripheral, without
// the allocator. Leave the pair allocated and untouched, so its transfers
// are not reset and the allocator does not hand it out again, and move to a
// fresh pair. Returns whether DMA is still usable.
bool movePair(const char *when) {
  const unsigned inSel = GDMA.channel[dma.pair].in.peri_sel.sel;
  const unsigned outSel = GDMA.channel[dma.pair].out.peri_sel.sel;
  snprintf(dmaFault, sizeof(dmaFault), "%s: pair %d reconnected to peripheral in=%u out=%u",
           when, dma.pair, inSel, outSel);
  Serial.printf("SD SPI DMA %s; leaving it to that driver.\n", dmaFault);
  dma.tx = dma.rx = nullptr;
  ++dma.moves;
  if (dma.moves > MaxPairMoves || !acquirePair()) {
    state.dma = false;
    Serial.println(F("SD SPI DMA disabled; using FIFO transfers."));
    return false;
  }
  Serial.printf("SD SPI DMA moved to GDMA pair %d.\n", dma.pair);
  return true;
}

// Checked before each DMA operation; a few register reads.
bool dmaUsable() {
  if (!state.dma) return false;
  return pairConnected() || movePair("before transfer");
}

// Records a fault with the descriptor, GDMA RX channel, and SPI DMA state,
// resets both channels for the retry, and selects FIFO transfers once
// faults reach MaxDmaFaults.
void recordDmaFault(const char *direction, const char *reason, uint32_t descriptorWord) {
  volatile auto &in = GDMA.channel[dma.pair].in;
  snprintf(dmaFault, sizeof(dmaFault),
           "%s: %s (dw0=%08lx ch=%d sel=%lu link=%08lx state=%08lx fifo=%08lx raw=%08lx spi=%08lx/%08lx)",
           direction, reason, static_cast<unsigned long>(descriptorWord), dma.pair,
           static_cast<unsigned long>(in.peri_sel.sel), static_cast<unsigned long>(in.link.val),
           static_cast<unsigned long>(in.state.val), static_cast<unsigned long>(in.infifo_status.val),
           static_cast<unsigned long>(in.int_raw.val), static_cast<unsigned long>(bus.dma_conf.val),
           static_cast<unsigned long>(bus.dma_int_raw.val));
  ++state.dmaFaults;
  state.dmaFaultPending = true;
  gdma_reset(dma.tx);
  gdma_reset(dma.rx);
  Serial.printf("SD SPI DMA fault %u/%u, %s\n", state.dmaFaults, MaxDmaFaults, dmaFault);
  if (state.dmaFaults >= MaxDmaFaults) {
    disableDma();
    Serial.println(F("SD SPI DMA disabled; using FIFO transfers."));
  }
}

// Last resort for a transaction that never ends: reset SPI2 and restore the
// configuration the Arduino HAL set up, so neither this driver nor touch
// waits forever on the busy flag. Pin routing lives in the GPIO matrix and
// survives the reset.
void resetBus() {
  const uint32_t ctrl = bus.ctrl.val;
  const uint32_t clock = bus.clock.val;
  const uint32_t user = bus.user.val;
  const uint32_t user1 = bus.user1.val;
  const uint32_t user2 = bus.user2.val;
  const uint32_t misc = bus.misc.val;
  const uint32_t dinMode = bus.din_mode.val;
  const uint32_t dinNum = bus.din_num.val;
  const uint32_t doutMode = bus.dout_mode.val;
  const uint32_t slave = bus.slave.val;
  const uint32_t clkGate = bus.clk_gate.val;
  periph_module_reset(PERIPH_SPI2_MODULE);
  bus.clk_gate.val = clkGate;
  bus.slave.val = slave;
  bus.ctrl.val = ctrl;
  bus.clock.val = clock;
  bus.user.val = user;
  bus.user1.val = user1;
  bus.user2.val = user2;
  bus.misc.val = misc;
  bus.din_mode.val = dinMode;
  bus.din_num.val = dinNum;
  bus.dout_mode.val = doutMode;
  bus.dma_conf.val = 0;
}

// Waits for the transaction and for GDMA to write back the receive
// descriptor, so all `length` bytes are in memory, then restores
// CPU-controlled mode. Any failure turns DMA off with the reason recorded.
bool finishDma(uint8_t slot, uint32_t length, const char *direction) {
  const uint32_t started = now();
  const uint32_t mhz = getCpuFrequencyMhz();
  while (bus.cmd.usr) {
    if (now() - started > DmaTransferTimeoutUs * mhz) break;
  }
  if (bus.cmd.usr) {
    // A master stalled on an empty DMA FIFO can finish from the CPU buffer.
    bus.dma_conf.dma_rx_ena = 0;
    bus.dma_conf.dma_tx_ena = 0;
    const uint32_t drained = now();
    while (bus.cmd.usr && now() - drained < DmaTransferTimeoutUs * mhz) {}
    const bool reset = bus.cmd.usr;
    if (reset) resetBus();
    if (!pairConnected()) {
      state.dmaFaultPending = true;
      movePair(direction);
      return false;
    }
    recordDmaFault(direction, reset ? "SPI stalled, bus reset" : "SPI stalled",
                   *reinterpret_cast<volatile uint32_t *>(&dma.rxDescriptors[slot]));
    return false;
  }
  volatile dma_descriptor_t *descriptor = &dma.rxDescriptors[slot];
  const uint32_t writeback = now();
  while (descriptor->dw0.owner == DMA_DESCRIPTOR_BUFFER_OWNER_DMA &&
         now() - writeback <= DmaWritebackTimeoutUs * mhz) {}
  bus.dma_conf.dma_rx_ena = 0;
  bus.dma_conf.dma_tx_ena = 0;
  const uint32_t word = *reinterpret_cast<volatile uint32_t *>(descriptor);
  if (!pairConnected()) {
    state.dmaFaultPending = true;
    movePair(direction);
    return false;
  }
  if (descriptor->dw0.owner == DMA_DESCRIPTOR_BUFFER_OWNER_DMA) {
    recordDmaFault(direction, "descriptor not written back", word);
    return false;
  }
  if (descriptor->dw0.length != length) {
    recordDmaFault(direction, "unexpected received length", word);
    return false;
  }
  return true;
}

bool checkDmaBlock(uint8_t *destination, uint8_t slot) {
  const uint8_t *source = dma.slots[slot];
  memcpy(destination, source, BlockBytes);
  if (!state.crc) return true;
  const uint16_t expected = static_cast<uint16_t>((source[BlockBytes] << 8) | source[BlockBytes + 1]);
  return updateCrc16(0, source, BlockBytes) == expected;
}

// Alternates two receive buffers: while GDMA fills one with block i, the CPU
// copies block i-1 out of the other and verifies its CRC.
bool receiveBlocksDma(uint8_t *buffer, uint32_t count) {
  uint8_t *pending = nullptr;
  uint8_t pendingSlot = 0;
  bool ok = true;
  for (uint32_t i = 0; ok && i < count; ++i) {
    if (!waitDataToken(i ? &cycles.gap : &cycles.access)) return false;
    const uint8_t slot = i & 1;
    const uint32_t dataStarted = now();
    startDma(dma.ones, BlockWithCrcBytes, slot);
    const bool previous = !pending || checkDmaBlock(pending, pendingSlot);
    const bool received = finishDma(slot, BlockWithCrcBytes, "receive");
    cycles.data += now() - dataStarted;
    ok = previous && received;
    pending = buffer + i * BlockBytes;
    pendingSlot = slot;
  }
  return ok && (!pending || checkDmaBlock(pending, pendingSlot));
}

bool receiveBlocksFifo(uint8_t *buffer, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    if (!receiveBlock(buffer + i * BlockBytes, BlockBytes, i ? &cycles.gap : &cycles.access)) {
      return false;
    }
  }
  return true;
}

bool sendBlockDma(uint8_t token, const uint8_t *source) {
  uint8_t *outgoing = dma.outgoing;
  outgoing[0] = token;
  memcpy(outgoing + 1, source, BlockBytes);
  const uint16_t crc = state.crc ? updateCrc16(0, source, BlockBytes) : 0xFFFF;
  outgoing[BlockBytes + 1] = static_cast<uint8_t>(crc >> 8);
  outgoing[BlockBytes + 2] = static_cast<uint8_t>(crc);
  startDma(outgoing, TokenBlockWithCrcBytes, 0);
  if (!finishDma(0, TokenBlockWithCrcBytes, "send")) return false;
  return (transferByte(0xFF) & 0x1F) == 0x05;
}

bool sendBlock(uint8_t token, const uint8_t *source) {
  return dmaUsable() ? sendBlockDma(token, source) : sendBlockFifo(token, source);
}

// Allocates a TX/RX pair and connects it to SPI2. Pair 0 is never used:
// on this firmware the hardware-AES DMA path reprogrammed channel 0 to AES
// while SD owned it, outside the allocator. A pair 0 that the allocator
// hands out is therefore kept allocated and idle, so no other driver gets it
// either, and the next pair is used.
bool acquirePair() {
  for (uint8_t attempt = 0; attempt < 2; ++attempt) {
    gdma_channel_alloc_config_t txConfig = {};
    txConfig.direction = GDMA_CHANNEL_DIRECTION_TX;
    gdma_channel_handle_t tx = nullptr;
    gdma_channel_handle_t rx = nullptr;
    if (gdma_new_ahb_channel(&txConfig, &tx) != ESP_OK) return false;
    gdma_channel_alloc_config_t rxConfig = {};
    rxConfig.direction = GDMA_CHANNEL_DIRECTION_RX;
    rxConfig.sibling_chan = tx;
    int group = 0;
    int pair = 0;
    if (gdma_new_ahb_channel(&rxConfig, &rx) != ESP_OK ||
        gdma_get_group_channel_id(rx, &group, &pair) != ESP_OK) {
      if (rx) gdma_del_channel(rx);
      gdma_del_channel(tx);
      return false;
    }
    if (pair == 0) continue;
    if (gdma_connect(tx, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_SPI, 2)) != ESP_OK ||
        gdma_connect(rx, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_SPI, 2)) != ESP_OK) {
      gdma_del_channel(rx);
      gdma_del_channel(tx);
      return false;
    }
    dma.tx = tx;
    dma.rx = rx;
    dma.pair = pair;
    return true;
  }
  return false;
}

bool allocateDma() {
  if (dma.ready) return dma.tx != nullptr;
  const size_t descriptorBytes = 3 * sizeof(dma_descriptor_t);
  const size_t bytes = descriptorBytes + 4 * DmaBufferBytes;
  uint8_t *memory = static_cast<uint8_t *>(
      heap_caps_aligned_calloc(4, 1, bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  if (!memory) return false;
  if (!acquirePair()) {
    heap_caps_free(memory);
    return false;
  }
  dma.txDescriptor = reinterpret_cast<dma_descriptor_t *>(memory);
  dma.rxDescriptors = dma.txDescriptor + 1;
  uint8_t *buffers = memory + descriptorBytes;
  dma.ones = buffers;
  dma.slots[0] = buffers + DmaBufferBytes;
  dma.slots[1] = buffers + 2 * DmaBufferBytes;
  dma.outgoing = buffers + 3 * DmaBufferBytes;
  memset(dma.ones, 0xFF, DmaBufferBytes);
  dma.ready = true;
  return true;
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
  bool ok = started && (dmaUsable() ? receiveBlocksDma(buffer, count)
                                    : receiveBlocksFifo(buffer, count));
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
    state.dmaFaultPending = false;
    beginBus();
    const bool ok = operation();
    endBus();
    if (ok) return true;
    ++state.retries;
    // A GDMA fault is retried as is; it says nothing about the clock.
    if (state.dmaFaultPending) continue;
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

// Reads a FIFO reference at the base clock, verifies the DMA path against it
// at that clock, then probes faster clocks with whichever path is selected.
uint8_t selectFastestLevel() {
  const uint8_t slowest = ClockLevels - 1;
  const size_t bytes = ProbeSectors * BlockBytes;
  uint8_t *reference = static_cast<uint8_t *>(ps_malloc(bytes));
  uint8_t *check = static_cast<uint8_t *>(ps_malloc(bytes));
  const bool wantDma = state.dma;
  state.dma = false;
  uint8_t chosen = slowest;
  if (reference && check) {
    state.level = slowest;
    beginBus();
    const bool haveReference = readOnce(0, reference, ProbeSectors);
    endBus();
    state.dma = wantDma && haveReference;
    if (state.dma && !probeLevel(slowest, reference, check)) {
      // A GDMA fault has already disabled DMA and logged why.
      if (!state.dmaFaultPending) {
        strlcpy(dmaFault, "probe: data differs from FIFO reference", sizeof(dmaFault));
      }
      Serial.println(F("SD SPI DMA probe failed; using FIFO transfers."));
      disableDma();
    }
    for (uint8_t level = state.fastestLevel; haveReference && level < slowest; ++level) {
      if (probeLevel(level, reference, check)) {
        chosen = level;
        break;
      }
      Serial.printf("SD SPI probe failed at %.2f MHz.\n", state.clocks[level] / 1000000.0f);
    }
  }
  state.level = chosen;
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
  dmaFault[0] = '\0';
  state.dmaFaults = 0;
  state.dmaFaultPending = false;
  state.dma = allocateDma();
  if (!state.dma) {
    strlcpy(dmaFault, "GDMA channels or DMA memory unavailable", sizeof(dmaFault));
    Serial.println(F("SD SPI DMA unavailable; using FIFO transfers."));
  }
  state.level = selectFastestLevel();

  state.attached = true;
  ff_diskio_register(pdrv, &DiskIo);
  Serial.printf("SD SPI driver: %.2f MHz, DMA %s (GDMA pair %d), High-Speed %s, CRC %s.\n",
                state.clocks[state.level] / 1000000.0f, state.dma ? "on" : "off",
                dma.tx ? dma.pair : -1,
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
  return {state.attached, state.highSpeed, state.crc, state.attached && state.dma, state.dmaFaults,
          dmaFault,
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
