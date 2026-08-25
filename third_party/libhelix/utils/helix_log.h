#pragma once
#include "ConfigHelix.h"

#if defined(USE_IDF_LOGGER)
#  include "helix_log_idf.h"
#else
#  include "helix_log_std.h"
#endif
