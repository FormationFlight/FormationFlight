#pragma once
//
// A LoRa channel: where it sits, how it is modulated, and the band rules it has
// to stay inside.
//
// The band is a setting (radios.lora_band), not a build flag. The chip is still
// compiled in, because that is what the hardware is, but which of the bands the
// chip can reach a node runs on is chosen at runtime: an SX127x board can run
// 433, 868 or 915, an LR1121 any of the four. lora_band.cpp holds the one
// channel per band and the reasoning behind each number.
//
// VERIFY THESE AGAINST THE RULES WHERE YOU FLY. They are researched defaults,
// not legal advice, and the allocations differ by region.
//
#include <cstddef>
#include <cstdint>

#include "airtime.h"
#include "hw_layout.h"

namespace ff {

struct LoraBand {
    uint16_t band = 0;        // 433, 868, 915 or 2400: the allocation, not the channel
    uint32_t frequency_hz = 0;
    float bandwidth_khz = 0.0f;
    uint8_t spreading_factor = 0;
    uint8_t coding_rate = 0;  // denominator: 5 means 4/5
    // SX1280/LR1121 only. Long interleaving spreads a burst of interference
    // across more codewords; it is a different on-air format, so every node on
    // the band has to agree.
    bool long_interleaving = false;
    uint16_t preamble_symbols = 8;
    uint8_t sync_word = 0x17;
    // The band's legal duty-cycle ceiling as a percentage, 0 for none. The rate
    // controller treats it as a hard floor on the beacon interval.
    uint8_t duty_cycle_pct = 0;
    // The legal ceiling for this band, in dBm at the antenna port. The TX power
    // setting and the board default are both capped to it.
    int8_t max_dbm = 30;

    bool valid() const { return band != 0; }
    bool highBand() const { return band == 2400; }
};

// Every band there is a preset for, in the order the web UI lists them.
constexpr uint16_t kLoraBands[] = {433, 868, 915, 2400};
constexpr size_t kLoraBandCount = sizeof(kLoraBands) / sizeof(kLoraBands[0]);

// The channel this project runs on `band`. False for a band it has none for.
bool loraBandPreset(uint16_t band, LoraBand& out);

// Whether `chip` can transmit on `band` at all.
bool chipSupportsBand(LoraChip chip, uint16_t band);

// Whether this board can: the chip, and the RF front end around it.
//
// ExpressLRS hardware is matched for 868/915 ("900") and 2.4 GHz, never 433:
// an SX127x or LR1121 there would tune 433 happily and put its power into a
// matching network built for twice the frequency. So a board described by an
// ELRS layout (elrs_hardware) is never offered 433. The hand-written targets
// in targets/*.ini are the ones that can be 433 boards.
bool boardSupportsBand(LoraChip chip, bool elrs_hardware, uint16_t band);

// The band a node runs on: the setting if there is one, else the default the
// image was flashed with, else the board's only band. 0 when none of those
// decide it -- a multi-band board with no band chosen -- and LoRa then stays
// off: a node that picks a band by itself picks the wrong one for half the
// world. A choice the board cannot run counts as no choice.
uint16_t resolveLoraBand(LoraChip chip, bool elrs_hardware, uint16_t configured,
                         uint16_t flashed_default);

// The allocation each band's channel has to fit in, edges included. Narrower
// than the whole band where the rules change inside it (868: the 869.40-869.65
// sub-band, the only EU one with the power and duty cycle to beacon in).
bool loraBandEdges(uint16_t band, double& low_hz, double& high_hz);

// Whether the channel sits wholly inside its allocation, and, for a fixed
// channel at 2.4 GHz, is at least 500 kHz wide (FCC 15.247(a)(2): ELRS gets away
// with narrower because it hops; nothing here does).
bool loraBandValid(const LoraBand& b, const char** err);

// For the rate controller. Long interleaving is costed as the same coding rate
// without it: the SX1280 datasheet's interleaved formula differs by a symbol or
// two either way, far inside what channel-load targeting cares about.
LoraParams loraAirtimeParams(const LoraBand& b);

// The stricter of two duty-cycle ceilings, where 0 means none.
uint8_t stricterDutyCycle(uint8_t a, uint8_t b);

}  // namespace ff
