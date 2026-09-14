#pragma once
//
// What the GPS module is actually saying, as opposed to whether we understood
// it.
//
// A receiver with a fix and a receiver that is not wired up look identical from
// the position API: both report no fix and zero satellites. They are completely
// different problems and the difference is invisible without counting what
// arrives on the wire. That gap cost real bench time, so the counters are part
// of the firmware rather than something to add back each time.
//
// The raw sniff is the part that settles arguments. A module can be talking
// perfectly and still be unintelligible to us - a protocol generation older
// than the message we asked for, NMEA where we expected binary - and no counter
// of successfully parsed frames can tell you that, because the number it
// reports is zero in every one of those cases.
//
#include <cstddef>
#include <cstdint>

namespace ff {

// Distinct UBX message types remembered. Small on purpose: a configured module
// sends one or two, and a module we failed to configure sends whatever its
// defaults are, which is also a short list.
constexpr size_t kGnssSeenTypes = 8;

struct GnssMsgCount {
    uint8_t cls = 0;
    uint8_t id = 0;
    uint32_t count = 0;
};

struct GnssLinkStats {
    // Every byte the UART handed us, whether or not it parsed. Zero here means
    // the module is silent, mis-wired, unpowered or at a baud we never tried,
    // and nothing further up is worth investigating until it is non-zero.
    uint32_t bytes = 0;
    // Checksum-valid UBX frames, and how many of those were the navigation
    // message the driver actually wants.
    uint32_t ubx_frames = 0;
    uint32_t nav_pvt = 0;
    // NMEA sentence starts. A module happily emitting NMEA while we count zero
    // UBX frames means our port configuration never took effect.
    uint32_t nmea = 0;
    // Times the driver gave up and restarted its baud sweep. Climbing means it
    // has never held a conversation for four seconds together.
    uint32_t sweeps = 0;
    uint32_t baud = 0;
    bool configured = false;
    // Milliseconds since the last byte and since the last usable fix message.
    uint32_t last_byte_age_ms = 0;
    uint32_t last_pvt_age_ms = 0;
    GnssMsgCount seen[kGnssSeenTypes];
};

// Implemented by a location source that owns a GPS UART.
class IGnssLink {
public:
    virtual ~IGnssLink() = default;
    virtual GnssLinkStats gnssStats(uint32_t now_ms) const = 0;
    // Copies the most recent raw bytes, oldest first. Returns how many were
    // written. This is the one that tells you what the module is really doing.
    virtual size_t gnssSniff(uint8_t* out, size_t cap) const = 0;
};

}  // namespace ff
