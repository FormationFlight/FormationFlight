#pragma once
#include <cstdint>

namespace ff {

// A received frame handed from a radio adapter's RX servicing to the main loop,
// which passes it to Node::onReceive(). Fixed-size so it lives in a lock-free
// ring with no allocation.
struct RxFrame {
    uint32_t timestamp_ms;
    int16_t rssi;
    uint8_t len;
    uint8_t data[64];
};

}  // namespace ff
