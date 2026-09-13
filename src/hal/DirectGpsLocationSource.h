#pragma once
#ifdef GNSS_ENABLED

#include <Arduino.h>

#include "gnss_link.h"
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
class DirectGpsLocationSource : public ILocationSource, public IGnssLink {
public:
    // rate_hz is the desired navigation rate; clamped to a sane u-blox range.
    void begin(uint8_t uart_index, int8_t pin_rx, int8_t pin_tx, uint8_t rate_hz = 10);
    void service();

    NodeLocation getLocation() override { return cached_; }

    bool configured() const { return state_ == Parse; }
    uint32_t currentBaud() const { return current_baud_; }

    // IGnssLink. See gnss_link.h for why a byte counter and a raw tap earn
    // their place in flight firmware.
    GnssLinkStats gnssStats(uint32_t now_ms) const override;
    size_t gnssSniff(uint8_t* out, size_t cap) const override;

private:
    enum State : uint8_t { Sweep, Parse };

    void setBaud(uint32_t baud);
    void sendConfig();
    void handlePvt();
    void handleLegacyNav(uint8_t id);
    void handleAck(bool acked);
    void noteByte(uint8_t b);
    void noteMessage(uint8_t cls, uint8_t id);

    // Last bytes off the wire, oldest overwritten. 192 is two NMEA sentences or
    // two NAV-PVT frames, which is enough to recognise either on sight.
    static constexpr size_t kSniffBytes = 192;
    uint8_t sniff_[kSniffBytes] = {0};
    size_t sniff_head_ = 0;
    size_t sniff_count_ = 0;

    GnssMsgCount seen_[kGnssSeenTypes];
    uint32_t bytes_ = 0;
    uint32_t ubx_frames_ = 0;
    uint32_t nav_pvt_ = 0;
    uint32_t nmea_ = 0;
    uint32_t sweeps_ = 0;
    uint32_t last_byte_ms_ = 0;
    uint8_t prev_byte_ = 0;

    // True once the module has told us it does not have NAV-PVT. Everything
    // before u-blox protocol 14 is in this camp, which includes the NEO-6M
    // fitted to a lot of T-Beams.
    bool legacy_ = false;
    // Set between asking for NAV-PVT and hearing back, so an unrelated NAK for
    // some other request is not mistaken for the answer.
    bool awaiting_pvt_ack_ = false;

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
