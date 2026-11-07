#include "GdmaRouting.h"

#include <Arduino.h>
#include <soc/gdma_struct.h>

namespace {

constexpr uint8_t PairCount = sizeof(GDMA.channel) / sizeof(GDMA.channel[0]);

const char *peripheralName(uint32_t selection) {
  switch (selection) {
    case 0: return "SPI2";
    case 1: return "SPI3";
    case 2: return "UHCI0";
    case 3: return "I2S0";
    case 4: return "I2S1";
    case 5: return "LCD_CAM";
    case 6: return "AES";
    case 7: return "SHA";
    case 8: return "ADC";
    case 9: return "RMT";
    case 63: return "-";
    default: return "?";
  }
}

}  // namespace

namespace GdmaRouting {

void log(const char *context) {
  Serial.print(F("GDMA routing context="));
  Serial.print(context ? context : "?");
  for (uint8_t pair = 0; pair < PairCount; ++pair) {
    const auto &channel = GDMA.channel[pair];
    Serial.printf(" %u=in:%s/out:%s,%s", pair,
                  peripheralName(channel.in.peri_sel.sel),
                  peripheralName(channel.out.peri_sel.sel),
                  channel.out.link.park ? "parked" : "running");
  }
  Serial.println();
}

}  // namespace GdmaRouting
