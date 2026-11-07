#pragma once

// Read-only snapshot of the ESP32-S3 AHB GDMA routing for serial diagnostics.
// I2S, SD SPI, and hardware AES/SHA all share the chip's five channel pairs;
// one line shows which peripheral each IN/OUT channel serves and whether its
// OUT link is parked, which is enough to tell a stalled speaker DMA from a
// rerouted one. It never changes a register.
namespace GdmaRouting {

// Prints "GDMA routing context=<context> 0=in:SPI2/out:SPI2,parked ...".
void log(const char *context);

}  // namespace GdmaRouting
