#pragma once
#include <cstdint>

namespace ff {

// A received frame handed from a radio driver's RX servicing to the main loop /
// RadioHub, which passes it to Node::onReceive(). Fixed-size so it lives in a
// lock-free ring with no allocation.
struct RxFrame {
    uint32_t timestamp_ms = 0;
    int16_t rssi = 0;
    uint8_t len = 0;
    uint8_t data[64] = {0};
};

}  // namespace ff
