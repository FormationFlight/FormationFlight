#pragma once

#include "hw_layout.h"

namespace ff {

// Fills `out` with this board's layout, in increasing order of precedence:
//
//   1. the pins compiled in by a legacy targets/*.ini env, if any;
//   2. the ExpressLRS layout appended to the firmware image
//      (scripts/elrs_targets.py configure);
//   3. /hardware.json on LittleFS, which wins over both, as it does in ELRS.
//
// LittleFS must already be mounted for (3). Never fails: a unified image with
// no trailer comes back with source None and no pins, and the caller decides
// what that means.
void loadHardwareLayout(HardwareLayout& out);

}  // namespace ff
