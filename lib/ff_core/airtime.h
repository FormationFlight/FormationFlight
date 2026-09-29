#pragma once
//
// LoRa time-on-air calculation (pure math).
//
// Used by the rate controller to size the beacon interval so that aggregate
// channel utilisation stays near a target. The radio driver computes the airtime
// for its configured modulation once at init and hands the value to RateController.
//
// Implements the standard Semtech time-on-air formula (AN1200.13). SX128x LoRa
// differs very slightly for some spreading factors; the approximation is well
// within tolerance for channel-load targeting.
//
#include <cstdint>
#include <cstddef>

namespace ff {

struct LoraParams {
    uint8_t spreading_factor;    // 5..12
    uint32_t bandwidth_hz;       // e.g. 250000
    uint8_t coding_rate_denom;   // 5..8 for 4/5..4/8
    uint16_t preamble_symbols;   // e.g. 8
    bool explicit_header;        // false => implicit header (required for SF6)
    bool crc_on;
    bool low_data_rate_optimize; // typically true for SF11/12 at narrow BW
};

// Time on air, in milliseconds, for a payload of the given size.
double loraAirtimeMs(const LoraParams& p, size_t payload_bytes);

}  // namespace ff
