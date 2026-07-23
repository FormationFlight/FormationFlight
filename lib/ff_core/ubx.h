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
    uint8_t num_sat = 0;
    int32_t lat = 0;           // deg * 1e7
    int32_t lon = 0;           // deg * 1e7
    int16_t alt_m = 0;         // metres MSL
    uint16_t speed_cms = 0;    // cm/s
    uint16_t course_ddeg = 0;  // decidegrees 0..3599
};

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
