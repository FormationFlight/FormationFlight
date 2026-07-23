#pragma once
#ifdef GNSS_ENABLED

#include <Arduino.h>

#include "node.h"
#include "ubx.h"

namespace ff {

// ILocationSource for a u-blox GPS wired directly to the node (e.g. the onboard
// module on a LilyGo T-Beam), fully non-blocking.
//
// Out of the box these modules emit NMEA at 1 Hz. begin()/service() run a small
// state machine that: sweeps candidate baud rates sending UBX-CFG-PRT to raise the
// module to a fast port speed, then sets a high navigation rate (UBX-CFG-RATE) and
// switches the module to a single compact binary fix message (UBX-NAV-PVT, NMEA
// off). From then on service() just drains whatever bytes are buffered through the
// UBX parser. If the module goes silent (e.g. it powered up after us), the sweep
// restarts automatically. Nothing ever blocks the loop.
class DirectGpsLocationSource : public ILocationSource {
public:
    // rate_hz is the desired navigation rate; clamped to a sane u-blox range.
    void begin(uint8_t uart_index, int8_t pin_rx, int8_t pin_tx, uint8_t rate_hz = 10);
    void service();

    NodeLocation getLocation() override { return cached_; }

    bool configured() const { return state_ == Parse; }
    uint32_t currentBaud() const { return current_baud_; }

private:
    enum State : uint8_t { Sweep, Parse };

    void setBaud(uint32_t baud);
    void sendConfig();
    void handlePvt();

    HardwareSerial* serial_ = nullptr;
    int8_t pin_rx_ = -1;
    int8_t pin_tx_ = -1;
    UbxParser parser_;
    NodeLocation cached_;

    State state_ = Sweep;
    uint16_t meas_ms_ = 100;
    uint32_t target_baud_ = 115200;
    uint32_t current_baud_ = 0;
    uint8_t sweep_idx_ = 0;
    uint32_t sweep_step_ms_ = 0;
    uint32_t last_pvt_ms_ = 0;
};

}  // namespace ff

#endif  // GNSS_ENABLED
