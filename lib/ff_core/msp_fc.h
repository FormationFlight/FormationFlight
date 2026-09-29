#pragma once
//
// MSP flight-controller protocol helpers: frame builders, reply decoders, and
// the box-mode bitmap mapping.
//
// This is the pure half of talking to an INAV/Betaflight FC over MSP. It knows
// the byte layouts and nothing else -- no serial port, no timing -- so every
// encoder and decoder is exercised on the host. The hardware adapter
// (hal/MspFcLink) owns the UART and the request schedule; it only ever calls
// into here to turn bytes into meaning and meaning into bytes.
//
// Layouts are verified against INAV's src/main/fc/fc_msp.c and the v1
// firmware's MSP.h structs (kept in src/lib/MSP as reference).
//
#include <cstddef>
#include <cstdint>

namespace ff {

// ---- Message ids ------------------------------------------------------------

constexpr uint8_t kMspFcVariant = 2;
constexpr uint8_t kMspFcVersion = 3;
constexpr uint8_t kMspStatus = 101;
constexpr uint8_t kMspRc = 105;
constexpr uint8_t kMspRawGps = 106;
constexpr uint8_t kMspAltitude = 109;
constexpr uint8_t kMspBoxIds = 119;
constexpr uint8_t kMspSetWp = 209;
constexpr uint8_t kMspSetHead = 211;

constexpr uint16_t kMsp2InavStatus = 0x2000;
constexpr uint16_t kMsp2InavMixer = 0x2010;
constexpr uint16_t kMsp2InavSetGvar = 0x2214;

constexpr size_t kMspMaxRcChannels = 16;
constexpr uint8_t kMspGpsNoFix = 0;

// MSP_SET_WP: waypoint 255 is INAV's "POSHOLD / follow-me" special waypoint.
constexpr uint8_t kMspFollowWaypoint = 255;
constexpr uint8_t kMspWpActionWaypoint = 1;

// ---- Frames -----------------------------------------------------------------

constexpr size_t kMspV1Overhead = 6;  // $ M < size id ... crc
constexpr size_t kMspV2Overhead = 9;  // $ X < flag id(2) size(2) ... crc

// Build an outbound MSP v1 frame ('$' 'M' '<'). A zero-length payload is a
// request. Returns the frame length, or 0 if cap is too small.
size_t buildMspV1(uint8_t id, const uint8_t* payload, size_t len, uint8_t* buf, size_t cap);

// Build an outbound MSP v2 frame ('$' 'X' '<', flag 0). Returns the frame
// length, or 0 if cap is too small.
size_t buildMspV2(uint16_t id, const uint8_t* payload, size_t len, uint8_t* buf, size_t cap);

// ---- Reply decoders ---------------------------------------------------------
// Each returns false when the payload is shorter than the message requires.

struct FcRawGps {
    uint8_t fix_type = 0;     // kMspGpsNoFix, 2D, 3D
    uint8_t num_sat = 0;
    int32_t lat = 0;          // deg * 1e7
    int32_t lon = 0;          // deg * 1e7
    int16_t alt_m = 0;        // metres MSL
    int16_t speed_cms = 0;    // cm/s
    int16_t course_ddeg = 0;  // decidegrees
    uint16_t hdop = 0;
};
bool decodeRawGps(const uint8_t* p, size_t len, FcRawGps& out);

// MSP_ALTITUDE: estimated home-relative altitude, cm.
bool decodeAltitudeCm(const uint8_t* p, size_t len, int32_t& out_cm);

// MSP_RC: channel pulse widths in microseconds. Returns the channel count
// decoded (0 if the payload is empty), at most cap.
size_t decodeRc(const uint8_t* p, size_t len, uint16_t* out, size_t cap);

struct FcVersion {
    uint8_t major = 0;
    uint8_t minor = 0;
    uint8_t patch = 0;
};
bool decodeFcVersion(const uint8_t* p, size_t len, FcVersion& out);

// MSP_FC_VARIANT: 4-char identifier ("INAV", "BTFL", ...), null-terminated.
bool decodeFcVariant(const uint8_t* p, size_t len, char out[5]);
inline bool fcVariantIsInav(const char v[5]) {
    return v[0] == 'I' && v[1] == 'N' && v[2] == 'A' && v[3] == 'V';
}

// INAV mixer platform type (flyingPlatformType_e), from MSP2_INAV_MIXER.
enum class FcPlatform : uint8_t {
    Multirotor = 0,
    Airplane = 1,
    Helicopter = 2,
    Tricopter = 3,
    Rover = 4,
    Boat = 5,
    // Not a wire value: "no MSP2_INAV_MIXER reply yet", so callers can tell
    // unanswered apart from a genuine Multirotor (0).
    Unknown = 0xFF,
};
bool decodeMixerPlatform(const uint8_t* p, size_t len, FcPlatform& out);

// ---- Active flight modes ----------------------------------------------------
//
// The FC reports active modes as a bitmask indexed by position in its own
// enabled-box list; MSP_BOXIDS gives each position's permanent id. We map that
// onto a fixed bitmap indexed by FcMode (the v1 MSP_MODE_* numbering) using the
// permanent-id table below, so callers never see the FC's per-config ordering.

enum FcMode : uint8_t {
    FC_MODE_ARM = 0,
    FC_MODE_ANGLE = 1,
    FC_MODE_HORIZON = 2,
    FC_MODE_NAVALTHOLD = 3,
    FC_MODE_MAG = 4,  // INAV "HEADING HOLD"
    FC_MODE_HEADFREE = 5,
    FC_MODE_HEADADJ = 6,
    FC_MODE_CAMSTAB = 7,
    FC_MODE_NAVRTH = 8,
    FC_MODE_NAVPOSHOLD = 9,
    FC_MODE_PASSTHRU = 10,
    FC_MODE_BEEPERON = 11,
    FC_MODE_LEDLOW = 12,
    FC_MODE_LLIGHTS = 13,
    FC_MODE_OSD = 14,
    FC_MODE_TELEMETRY = 15,
    FC_MODE_GTUNE = 16,
    FC_MODE_SONAR = 17,
    FC_MODE_BLACKBOX = 18,
    FC_MODE_FAILSAFE = 19,
    FC_MODE_NAVWP = 20,
    FC_MODE_AIRMODE = 21,
    FC_MODE_HOMERESET = 22,
    FC_MODE_GCSNAV = 23,
    FC_MODE_HEADINGLOCK = 24,
    FC_MODE_SURFACE = 25,
    FC_MODE_FLAPERON = 26,
    FC_MODE_TURNASSIST = 27,
    FC_MODE_NAVLAUNCH = 28,
    FC_MODE_AUTOTRIM = 29,
    kFcModeCount = 30,
};

// Permanent box id for each FcMode (mixed Cleanflight/INAV numbering, as v1).
extern const uint8_t kFcBoxPermanentIds[kFcModeCount];

// Locate the mode-flag bitmask inside a reply. MSP_STATUS carries a fixed 32-bit
// mask; MSP2_INAV_STATUS carries the FC's full-width mask (every remaining byte
// after the fixed header), which is the only way to see boxes past index 31 on
// an INAV build with many modes enabled. Returns false if too short.
bool statusModeFlags(const uint8_t* p, size_t len, const uint8_t*& flags, size_t& flag_len);
bool inavStatusModeFlags(const uint8_t* p, size_t len, const uint8_t*& flags, size_t& flag_len);

// INAV armingFlags from MSP2_INAV_STATUS (bit 2 = ARMED). More reliable than the
// ARM box bit, which only says the switch is on, not that arming succeeded.
bool inavStatusArmed(const uint8_t* p, size_t len, bool& out_armed);

// Map (mode-flag bitmask, MSP_BOXIDS reply) -> FcMode bitmap.
uint32_t decodeActiveModes(const uint8_t* flags, size_t flag_len, const uint8_t* box_ids,
                           size_t box_id_count);

inline bool modeActive(uint32_t modes, FcMode m) { return ((modes >> m) & 1u) != 0; }

// ---- Command payloads -------------------------------------------------------

constexpr size_t kMspSetWpPayloadSize = 21;
constexpr size_t kMspSetHeadPayloadSize = 2;
constexpr size_t kMspSetGvarPayloadSize = 5;

// MSP_SET_WP payload. alt is cm; p1 is heading (deg) for waypoint 255, cruise
// speed for mission waypoints. Returns bytes written, or 0 if cap too small.
size_t encodeSetWp(uint8_t wp_number, uint8_t action, int32_t lat, int32_t lon, int32_t alt_cm,
                   int16_t p1, int16_t p2, int16_t p3, uint8_t flag, uint8_t* buf, size_t cap);

// MSP_SET_HEAD payload (heading-hold target, degrees).
size_t encodeSetHead(int16_t heading_deg, uint8_t* buf, size_t cap);

// MSP2_INAV_SET_GVAR payload (INAV 9+): index 0-7, int32 value.
size_t encodeSetGvar(uint8_t index, int32_t value, uint8_t* buf, size_t cap);

}  // namespace ff
