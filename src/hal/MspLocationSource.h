#pragma once
#include <Stream.h>

#include "../lib/MSP/MSP.h"
#include "msp_parser.h"
#include "node.h"

namespace ff {

// ILocationSource backed by an MSP flight controller over serial, fully
// non-blocking.
//
// service() (called every main loop) does two cheap things: drain whatever bytes
// are already buffered on the serial port through the incremental MspParser, and
// -- on a rate-limited schedule -- send an MSP_RAW_GPS request (a non-blocking
// buffered write). A decoded fix updates the cache; getLocation() just returns it.
// Nothing ever waits on the serial port, so the beacon path is never stalled.
class MspLocationSource : public ILocationSource {
public:
    void begin(Stream& serial, uint32_t request_interval_ms = 200);
    void service();

    NodeLocation getLocation() override { return cached_; }

private:
    void handleFrame();

    Stream* stream_ = nullptr;
    MSP msp_;             // used only for the non-blocking send()
    MspParser parser_;
    NodeLocation cached_;
    uint32_t request_interval_ms_ = 200;
    uint32_t last_request_ms_ = 0;
    uint32_t last_fix_ms_ = 0;
    uint32_t stale_after_ms_ = 2000;  // drop validity if the FC goes quiet
    bool started_ = false;
    bool ever_fixed_ = false;
};

}  // namespace ff
