#pragma once
//
// CRC-8/DVB-S2, the checksum MSP v2 frames carry. Shared by the frame builders
// (msp_fc, msp_radar) and the incremental parser.
//
#include <cstdint>

namespace ff {

inline uint8_t mspCrc8DvbS2(uint8_t crc, uint8_t a) {
    crc ^= a;
    for (int i = 0; i < 8; i++) {
        crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0xD5)
                           : static_cast<uint8_t>(crc << 1);
    }
    return crc;
}

}  // namespace ff
