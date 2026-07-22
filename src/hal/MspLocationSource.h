#pragma once
#include <Stream.h>

#include "../lib/MSP/MSP.h"
#include "node.h"

namespace ff {

// ILocationSource backed by an MSP flight controller over serial.
//
// getLocation() must be cheap (it is called from the beacon path), so the actual
// MSP request -- which blocks briefly on the serial timeout -- is done on a
// rate-limited poll in service(), driven from the main loop. getLocation() just
// returns the last cached fix. (Making the MSP exchange fully non-blocking is a
// later refinement; the cache keeps the beacon path unblocked in the meantime.)
class MspLocationSource : public ILocationSource {
public:
    void begin(Stream& serial, uint32_t poll_interval_ms = 200);
    void service();

    NodeLocation getLocation() override { return cached_; }

private:
    MSP msp_;
    NodeLocation cached_;
    uint32_t poll_interval_ms_ = 200;
    uint32_t last_poll_ms_ = 0;
    bool started_ = false;
};

}  // namespace ff
