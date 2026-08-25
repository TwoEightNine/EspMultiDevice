#pragma once

// Arduino's sketch builder does not consistently forward project include
// paths into sketch-root C adapters. Keep Helix's basename include resolvable
// from the AAC source directory.
#include "../ConfigHelix.h"
