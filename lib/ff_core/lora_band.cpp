#include "lora_band.h"

namespace ff {

// v2 is single-channel by design: ALOHA replaced the TDMA machine, and nothing
// in it hops. That has a regulatory consequence worth stating plainly, because
// it is the opposite of how ExpressLRS is allowed to operate. ELRS is legal in
// the US via the frequency-hopping route (FCC 15.247(a)(1)), which lets it use
// narrow channels. A fixed channel has to take the digital-modulation route,
// 15.247(a)(2), which requires a 6 dB bandwidth of at least 500 kHz. So the
// bandwidths here are chosen partly for link budget and partly because going
// narrower would put a fixed-channel system outside the rule ELRS never needed.
//
// Every channel below is placed to sit wholly inside its band with margin, away
// from the obvious congestion, and - for 2.4 GHz - away from our OWN ESP-NOW
// radio, which is on the same board a couple of centimetres from the antenna.
//
// Spreading factor is chosen per band against its duty-cycle ceiling, not
// uniformly. Where there is no ceiling (915, 2400) airtime is cheap and it is
// worth spending on range. Where there is one (433, 868) every extra millisecond
// of airtime directly slows the beacon, and those bands are already the most
// sensitive of the four because their channels are narrower, so they stay at
// SF7 4/5.
bool loraBandPreset(uint16_t band, LoraBand& out) {
    LoraBand b;
    b.band = band;
    switch (band) {
        case 433:
            // 433.05-434.79 is the ITU Region 1 ISM allocation; in the US this
            // is 70 cm, amateur-primary, so a licence is what makes it legal
            // there. 434.4 keeps clear of 433.92, the default centre for
            // essentially every cheap remote and doorbell. 250 kHz rather than
            // 500: the whole band is only 1.74 MHz wide.
            b.frequency_hz = 434400000;
            b.bandwidth_khz = 250.0f;
            b.spreading_factor = 7;
            b.coding_rate = 5;
            b.duty_cycle_pct = 10;
            b.max_dbm = 10;  // 10 mW ERP, the Region 1 ISM limit
            break;
        case 868:
            // 869.40-869.65 is the only EU sub-band that permits 500 mW ERP and
            // 10% duty, which makes it the only one that can carry a useful
            // beacon rate. The previous 868.5 at 500 kHz spanned 868.25-868.75
            // and straddled the 868.6 boundary, so it sat in two sub-bands with
            // different power and duty limits at once. 125 kHz fits inside
            // 869.40-869.65 with margin; 250 would fill it edge to edge.
            b.frequency_hz = 869525000;
            b.bandwidth_khz = 125.0f;
            b.spreading_factor = 7;
            b.coding_rate = 5;
            b.duty_cycle_pct = 10;
            b.max_dbm = 27;  // 500 mW ERP in this sub-band
            break;
        case 915:
            // 902-928 US ISM. 915.000 was the worst available choice: dead
            // centre of the band, inside the LoRaWAN US915 uplink block
            // (902.3-914.9), and the frequency every other fixed-channel product
            // defaults to. 920.0 sits in the gap between that uplink block and
            // the LoRaWAN downlink block (923.3-927.5).
            //
            // SF8 rather than SF7, and 4/7: there is no duty-cycle ceiling here
            // and the airtime is only 43 ms, so the 2.5 dB is nearly free.
            // 902-928 is full of frequency-hopping traffic, which is burst
            // interference, which is what the extra coding rate is for.
            b.frequency_hz = 920000000;
            b.bandwidth_khz = 500.0f;
            b.spreading_factor = 8;
            b.coding_rate = 7;
            b.max_dbm = 30;  // 1 W conducted under 15.247(b)(3)
            break;
        case 2400:
            // 2400-2483.5 ISM. The previous 2400.000 centre put HALF the channel
            // below the band edge, and sat directly on top of our own ESP-NOW
            // radio: WiFi channel 1 occupies roughly 2401-2423, on the same
            // board. 2476.0 is inside the band with ~7 MHz of margin, above the
            // top of US WiFi channel 11 (2473), and ~60 MHz clear of ESP-NOW on
            // channel 1. 812.5 kHz rather than 406.25 so a fixed channel meets
            // the 500 kHz minimum 6 dB bandwidth; that is not optional, so the
            // link budget has to be found in the spreading factor instead.
            //
            // SF8, not SF5. 2.4 GHz is the weak link by a wide margin: at 10 dBm
            // into a compromised antenna it was reaching about a sixth of the
            // distance the sub-GHz bands do, partly from 8.6 dB more path loss
            // than 915 MHz and partly from SF5 being 7.5 dB less sensitive than
            // SF8. SF8 puts it at -118.4 dBm, the same sensitivity class as the
            // sub-GHz radios, for 30 ms of airtime. That trade is only
            // affordable because of what ALOHA changed: the rate controller
            // sizes each radio's interval from its own airtime, and ESP-NOW
            // carries the fast close-in updates, so LoRa can be the slow,
            // long-range layer it should have been.
            //
            // 4/8 with long interleaving rather than 4/5, because this band is
            // full of WiFi. Coding rate does not move the demodulation floor at
            // all - that is set by spreading factor and bandwidth - but it is
            // what survives a burst of interference rather than a weak signal,
            // and interleaving spreads the burst across more codewords.
            //
            // This used to say CR 4/7 with long interleaving, which the SX1280
            // does not have (LI comes in 4/5, 4/6 and 4/8). RadioLib 5 wrote the
            // 7 straight into the register, where it means LI 4/8, so 4/8 LI is
            // what every v2 2.4 GHz node has actually been transmitting.
            // RadioLib 7 rejects the old request and would have fallen back to
            // plain 4/7: a different on-air format. Stating it as it is keeps
            // existing nodes talking, and the LR1121 has the same mode.
            b.frequency_hz = 2476000000u;
            b.bandwidth_khz = 812.5f;
            b.spreading_factor = 8;
            b.coding_rate = 8;
            b.long_interleaving = true;
            b.preamble_symbols = 12;  // what the SX1280 has always used here
            // The US figure. In the EU the limit is 100 mW EIRP, so set
            // radios.lora_power_dbm to 20 or below there.
            b.max_dbm = 30;
            break;
        default:
            return false;
    }
    out = b;
    return true;
}

bool chipSupportsBand(LoraChip chip, uint16_t band) {
    switch (chip) {
        case LoraChip::SX127x:
            // RadioLib's SX1276 driver tunes 137-1020 MHz.
            return band == 433 || band == 868 || band == 915;
        case LoraChip::SX128x:
            return band == 2400;
        case LoraChip::LR1121:
            return band == 433 || band == 868 || band == 915 || band == 2400;
    }
    return false;
}

bool boardSupportsBand(LoraChip chip, bool elrs_hardware, uint16_t band) {
    if (elrs_hardware && band == 433) {
        return false;
    }
    return chipSupportsBand(chip, band);
}

uint16_t resolveLoraBand(LoraChip chip, bool elrs_hardware, uint16_t configured,
                         uint16_t flashed_default) {
    if (configured != 0 && boardSupportsBand(chip, elrs_hardware, configured)) {
        return configured;
    }
    if (flashed_default != 0 && boardSupportsBand(chip, elrs_hardware, flashed_default)) {
        return flashed_default;
    }
    uint16_t only = 0;
    for (uint16_t band : kLoraBands) {
        if (boardSupportsBand(chip, elrs_hardware, band)) {
            if (only != 0) {
                return 0;  // more than one: someone has to choose
            }
            only = band;
        }
    }
    return only;
}

bool loraBandEdges(uint16_t band, double& low_hz, double& high_hz) {
    switch (band) {
        case 433:
            low_hz = 433.05e6;
            high_hz = 434.79e6;
            return true;
        case 868:
            low_hz = 869.40e6;
            high_hz = 869.65e6;
            return true;
        case 915:
            low_hz = 902.0e6;
            high_hz = 928.0e6;
            return true;
        case 2400:
            low_hz = 2400.0e6;
            high_hz = 2483.5e6;
            return true;
        default:
            return false;
    }
}

bool loraBandValid(const LoraBand& b, const char** err) {
    double low, high;
    if (!loraBandEdges(b.band, low, high)) {
        *err = "unknown LoRa band";
        return false;
    }
    const double half_bw = b.bandwidth_khz * 1000.0 / 2.0;
    if (b.frequency_hz - half_bw < low) {
        *err = "LoRa channel extends below its band";
        return false;
    }
    if (b.frequency_hz + half_bw > high) {
        *err = "LoRa channel extends above its band";
        return false;
    }
    if (b.highBand() && b.bandwidth_khz < 500.0f) {
        *err = "a fixed-channel 2.4 GHz system needs at least 500 kHz of bandwidth";
        return false;
    }
    return true;
}

LoraParams loraAirtimeParams(const LoraBand& b) {
    LoraParams p{};
    p.spreading_factor = b.spreading_factor;
    p.bandwidth_hz = static_cast<uint32_t>(b.bandwidth_khz * 1000.0f);
    p.coding_rate_denom = b.coding_rate;
    p.preamble_symbols = b.preamble_symbols;
    p.explicit_header = true;
    p.crc_on = true;
    p.low_data_rate_optimize = false;
    return p;
}

uint8_t stricterDutyCycle(uint8_t a, uint8_t b) {
    if (a == 0) {
        return b;
    }
    if (b == 0) {
        return a;
    }
    return a < b ? a : b;
}

}  // namespace ff
