#pragma once

#include <Arduino.h>

namespace DisplayOrientation {

// The complete LCD/touch module is mounted 180 degrees from the ILI9488's
// native portrait orientation. Keep every caller on the same TFT_eSPI
// rotations so the logical UI remains upright relative to the enclosure.
static constexpr uint8_t Portrait = 2;
static constexpr uint8_t Landscape = 3;

}  // namespace DisplayOrientation
