#pragma once
//
// u-blox UBX protocol: a non-blocking frame parser, a NAV-PVT decoder, and
// builders for the configuration messages used to crank an attached GPS up to a
// high update rate.
//
// FPV GPS modules are almost universally u-blox. Out of the box they typically
// emit NMEA at 1 Hz -- far too slow and verbose for fresh position beacons. The
// builders here produce the UBX-CFG messages to (a) raise the port baud so a fast
// stream fits, (b) set the navigation rate (e.g. 10 Hz), and (c) switch the module
// to a single compact binary message (UBX-NAV-PVT) with NMEA turned off.
//
// UBX framing:  B5 62 | class | id | len(2 LE) | payload | ck_a ck_b
// where ck_a/ck_b are the 8-bit Fletcher checksum over class..payload.
//
// Pure and host-testable: no serial or hardware here.
//
#include <cstddef>
#include <cstdint>

namespace ff {

constexpr uint8_t kUbxClassNav = 0x01;
constexpr uint8_t kUbxIdNavPvt = 0x07;

// The legacy navigation set.
//
// NAV-PVT is a single message carrying everything, and it arrived in u-blox
// protocol version 14. Anything older -- a NEO-6M, which is what a great many
// T-Beams are actually fitted with -- does not have it, NAKs the CFG-MSG that
// asks for it, and then sits there emitting nothing while the receiver is
// perfectly happy and holding a fix. Four older messages cover the same ground.
constexpr uint8_t kUbxIdNavPosllh = 0x02;  // lat, lon, altitude
constexpr uint8_t kUbxIdNavSol = 0x06;     // fix type, satellites used
constexpr uint8_t kUbxIdNavVelned = 0x12;  // ground speed, heading
constexpr uint8_t kUbxIdNavDop = 0x04;     // dilution of precision

// Acknowledgements. The NAK is load-bearing: it is how a module tells us it has
// never heard of the message we just asked for, and it is the only warning we
// get before an indefinite silence that looks exactly like bad wiring.
constexpr uint8_t kUbxClassAck = 0x05;
constexpr uint8_t kUbxIdAckNak = 0x00;
constexpr uint8_t kUbxIdAckAck = 0x01;
constexpr uint8_t kUbxClassCfg = 0x06;
constexpr uint8_t kUbxIdCfgPrt = 0x00;
constexpr uint8_t kUbxIdCfgMsg = 0x01;
constexpr uint8_t kUbxIdCfgRate = 0x08;

// NMEA standard message class, whose messages we silence.
constexpr uint8_t kUbxClassNmea = 0xF0;

constexpr uint16_t kUbxMaxPayload = 128;  // NAV-PVT is 92 bytes

// Decoded subset of UBX-NAV-PVT, already in the wire protocol's units.
struct UbxFix {
    bool valid = false;
    // NAV-PVT's own fixType byte: 0 none, 1 dead reckoning, 2 2D, 3 3D,
    // 4 GNSS+DR, 5 time only. Reported as the receiver sent it, so a caller can
    // tell a 2D fix from a 3D one; `valid` stays the summary judgement, which
    // also requires the gnssFixOK flag the receiver sets separately.
    uint8_t fix_type = 0;
    uint8_t num_sat = 0;
    int32_t lat = 0;           // deg * 1e7
    int32_t lon = 0;           // deg * 1e7
    int16_t alt_m = 0;         // metres MSL
    uint16_t speed_cms = 0;    // cm/s
    uint16_t course_ddeg = 0;  // decidegrees 0..3599
};

// UBX-NAV-POSLLH: position only.
struct UbxPosLlh {
    int32_t lat = 0;   // deg * 1e7
    int32_t lon = 0;   // deg * 1e7
    int16_t alt_m = 0; // metres MSL
};

// UBX-NAV-SOL: fix quality only.
struct UbxSol {
    bool valid = false;
    uint8_t fix_type = 0;  // same numbering as NAV-PVT: 0 none, 2 2D, 3 3D
    uint8_t num_sat = 0;
};

// UBX-NAV-VELNED: motion only.
struct UbxVelNed {
    uint16_t speed_cms = 0;
    uint16_t course_ddeg = 0;  // 0..3599
};

bool decodeNavPosllh(const uint8_t* p, uint16_t len, UbxPosLlh& out);
bool decodeNavSol(const uint8_t* p, uint16_t len, UbxSol& out);
bool decodeNavVelned(const uint8_t* p, uint16_t len, UbxVelNed& out);
// Horizontal dilution of precision, x100, matching NodeLocation::hdop.
bool decodeNavDop(const uint8_t* p, uint16_t len, uint16_t& hdop_x100);
// The class and id of the message being acknowledged or rejected.
bool decodeAck(const uint8_t* p, uint16_t len, uint8_t& cls, uint8_t& id);

// Enables the legacy navigation set on `buf`, which must hold at least
// 4 * (8 + 3) bytes. Returns the total length written.
size_t buildLegacyNavConfig(uint8_t* buf, size_t cap);

class UbxParser {
public:
    // Feed one byte. Returns true exactly when it completes a checksum-valid
    // frame; msgClass()/msgId()/length()/payload() are then readable.
    bool feed(uint8_t b);

    uint8_t msgClass() const { return class_; }
    uint8_t msgId() const { return id_; }
    uint16_t length() const { return len_; }
    const uint8_t* payload() const { return payload_; }

    void reset();

private:
    enum State : uint8_t { Sync1, Sync2, Class, Id, LenLo, LenHi, Payload, CkA, CkB };

    State state_ = Sync1;
    uint8_t class_ = 0;
    uint8_t id_ = 0;
    uint16_t len_ = 0;
    uint16_t idx_ = 0;
    uint8_t ck_a_ = 0;
    uint8_t ck_b_ = 0;
    uint8_t rx_ck_a_ = 0;
    bool oversize_ = false;
    uint8_t payload_[kUbxMaxPayload] = {0};
};

// 8-bit Fletcher checksum over class,id,len(2),payload.
void ubxChecksum(const uint8_t* data, size_t len, uint8_t& ck_a, uint8_t& ck_b);

// Decode a UBX-NAV-PVT payload into UbxFix. Returns false if too short.
bool decodeNavPvt(const uint8_t* payload, uint16_t len, UbxFix& out);

// Message builders: each writes a full UBX frame to buf and returns its length,
// or 0 if cap is too small.
size_t buildCfgRate(uint16_t meas_ms, uint8_t* buf, size_t cap);
size_t buildCfgPrtUart(uint32_t baud, uint8_t* buf, size_t cap);
size_t buildCfgMsg(uint8_t msg_class, uint8_t msg_id, uint8_t rate, uint8_t* buf,
                   size_t cap);

}  // namespace ff
