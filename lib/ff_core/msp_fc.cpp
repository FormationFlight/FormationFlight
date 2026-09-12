#include "msp_fc.h"

#include "msp_crc.h"
#include "wire.h"

namespace ff {

const uint8_t kFcBoxPermanentIds[kFcModeCount] = {
    0,   //  0: ARM
    1,   //  1: ANGLE
    2,   //  2: HORIZON
    3,   //  3: NAVALTHOLD (cleanflight BARO)
    5,   //  4: MAG / HEADING HOLD
    6,   //  5: HEADFREE
    7,   //  6: HEADADJ
    8,   //  7: CAMSTAB
    10,  //  8: NAVRTH (cleanflight GPSHOME)
    11,  //  9: NAVPOSHOLD (cleanflight GPSHOLD)
    12,  // 10: PASSTHRU
    13,  // 11: BEEPERON
    15,  // 12: LEDLOW
    16,  // 13: LLIGHTS
    19,  // 14: OSD
    20,  // 15: TELEMETRY
    21,  // 16: GTUNE
    22,  // 17: SONAR
    26,  // 18: BLACKBOX
    27,  // 19: FAILSAFE
    28,  // 20: NAVWP (cleanflight AIRMODE)
    29,  // 21: AIRMODE (cleanflight DISABLE3DSWITCH)
    30,  // 22: HOMERESET (cleanflight FPVANGLEMIX)
    31,  // 23: GCSNAV (cleanflight BLACKBOXERASE)
    32,  // 24: HEADINGLOCK
    33,  // 25: SURFACE
    34,  // 26: FLAPERON
    35,  // 27: TURNASSIST
    36,  // 28: NAVLAUNCH
    37,  // 29: AUTOTRIM
};

// ---- Frames -----------------------------------------------------------------

size_t buildMspV1(uint8_t id, const uint8_t* payload, size_t len, uint8_t* buf, size_t cap) {
    if (len > 255 || cap < kMspV1Overhead + len) {
        return 0;
    }
    uint8_t* out = buf;
    wire::put_u8(out, '$');
    wire::put_u8(out, 'M');
    wire::put_u8(out, '<');
    wire::put_u8(out, static_cast<uint8_t>(len));
    wire::put_u8(out, id);
    uint8_t crc = static_cast<uint8_t>(len) ^ id;
    for (size_t i = 0; i < len; i++) {
        wire::put_u8(out, payload[i]);
        crc ^= payload[i];
    }
    wire::put_u8(out, crc);
    return static_cast<size_t>(out - buf);
}

size_t buildMspV2(uint16_t id, const uint8_t* payload, size_t len, uint8_t* buf, size_t cap) {
    if (len > 0xFFFF || cap < kMspV2Overhead + len) {
        return 0;
    }
    uint8_t* out = buf;
    wire::put_u8(out, '$');
    wire::put_u8(out, 'X');
    wire::put_u8(out, '<');
    wire::put_u8(out, 0);  // flag
    wire::put_u16(out, id);
    wire::put_u16(out, static_cast<uint16_t>(len));

    uint8_t crc = 0;
    crc = mspCrc8DvbS2(crc, 0);
    crc = mspCrc8DvbS2(crc, static_cast<uint8_t>(id & 0xFF));
    crc = mspCrc8DvbS2(crc, static_cast<uint8_t>(id >> 8));
    crc = mspCrc8DvbS2(crc, static_cast<uint8_t>(len & 0xFF));
    crc = mspCrc8DvbS2(crc, static_cast<uint8_t>(len >> 8));
    for (size_t i = 0; i < len; i++) {
        wire::put_u8(out, payload[i]);
        crc = mspCrc8DvbS2(crc, payload[i]);
    }
    wire::put_u8(out, crc);
    return static_cast<size_t>(out - buf);
}

// ---- Reply decoders ---------------------------------------------------------

bool decodeRawGps(const uint8_t* p, size_t len, FcRawGps& out) {
    if (len < 18) {
        return false;
    }
    out.fix_type = wire::get_u8(p);
    out.num_sat = wire::get_u8(p);
    out.lat = wire::get_i32(p);
    out.lon = wire::get_i32(p);
    out.alt_m = wire::get_i16(p);
    out.speed_cms = wire::get_i16(p);
    out.course_ddeg = wire::get_i16(p);
    out.hdop = wire::get_u16(p);
    return true;
}

bool decodeAltitudeCm(const uint8_t* p, size_t len, int32_t& out_cm) {
    if (len < 4) {
        return false;
    }
    out_cm = wire::get_i32(p);
    return true;
}

size_t decodeRc(const uint8_t* p, size_t len, uint16_t* out, size_t cap) {
    size_t n = len / 2;
    if (n > cap) {
        n = cap;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = wire::get_u16(p);
    }
    return n;
}

bool decodeFcVersion(const uint8_t* p, size_t len, FcVersion& out) {
    if (len < 3) {
        return false;
    }
    out.major = p[0];
    out.minor = p[1];
    out.patch = p[2];
    return true;
}

bool decodeFcVariant(const uint8_t* p, size_t len, char out[5]) {
    if (len < 4) {
        return false;
    }
    for (size_t i = 0; i < 4; i++) {
        out[i] = static_cast<char>(p[i]);
    }
    out[4] = '\0';
    return true;
}

bool decodeMixerPlatform(const uint8_t* p, size_t len, FcPlatform& out) {
    // motorDirectionInverted, reserved, motorstopOnLow, platformType, ...
    if (len < 4) {
        return false;
    }
    out = static_cast<FcPlatform>(p[3]);
    return true;
}

// ---- Active flight modes ----------------------------------------------------

bool statusModeFlags(const uint8_t* p, size_t len, const uint8_t*& flags, size_t& flag_len) {
    // cycleTime(2) i2cErrors(2) sensors(2) flightModeFlags(4) profile(1)
    if (len < 10) {
        return false;
    }
    flags = p + 6;
    flag_len = 4;
    return true;
}

bool inavStatusModeFlags(const uint8_t* p, size_t len, const uint8_t*& flags, size_t& flag_len) {
    // cycleTime(2) i2cErrors(2) sensors(2) cpuLoad(2) profiles(1) armingFlags(4)
    // boxModeFlags(remaining; INAV 7+ appends one mixerProfile byte, which
    // decodeActiveModes ignores because it lies past the MSP_BOXIDS count).
    if (len < 13) {
        return false;
    }
    flags = p + 13;
    flag_len = len - 13;
    return true;
}

bool inavStatusArmed(const uint8_t* p, size_t len, bool& out_armed) {
    if (len < 13) {
        return false;
    }
    const uint8_t* q = p + 9;
    const uint32_t arming_flags = wire::get_u32(q);
    out_armed = (arming_flags & (1u << 2)) != 0;  // ARMED
    return true;
}

uint32_t decodeActiveModes(const uint8_t* flags, size_t flag_len, const uint8_t* box_ids,
                           size_t box_id_count) {
    uint32_t modes = 0;
    for (size_t i = 0; i < box_id_count; i++) {
        const size_t byte = i / 8;
        if (byte >= flag_len) {
            break;
        }
        if ((flags[byte] & (1u << (i % 8))) == 0) {
            continue;
        }
        for (size_t j = 0; j < kFcModeCount; j++) {
            if (kFcBoxPermanentIds[j] == box_ids[i]) {
                modes |= (1u << j);
                break;
            }
        }
    }
    return modes;
}

// ---- Command payloads -------------------------------------------------------

size_t encodeSetWp(uint8_t wp_number, uint8_t action, int32_t lat, int32_t lon, int32_t alt_cm,
                   int16_t p1, int16_t p2, int16_t p3, uint8_t flag, uint8_t* buf, size_t cap) {
    if (cap < kMspSetWpPayloadSize) {
        return 0;
    }
    uint8_t* out = buf;
    wire::put_u8(out, wp_number);
    wire::put_u8(out, action);
    wire::put_i32(out, lat);
    wire::put_i32(out, lon);
    wire::put_i32(out, alt_cm);
    wire::put_i16(out, p1);
    wire::put_i16(out, p2);
    wire::put_i16(out, p3);
    wire::put_u8(out, flag);
    return static_cast<size_t>(out - buf);
}

size_t encodeSetHead(int16_t heading_deg, uint8_t* buf, size_t cap) {
    if (cap < kMspSetHeadPayloadSize) {
        return 0;
    }
    uint8_t* out = buf;
    wire::put_i16(out, heading_deg);
    return static_cast<size_t>(out - buf);
}

size_t encodeSetGvar(uint8_t index, int32_t value, uint8_t* buf, size_t cap) {
    if (cap < kMspSetGvarPayloadSize) {
        return 0;
    }
    uint8_t* out = buf;
    wire::put_u8(out, index);
    wire::put_i32(out, value);
    return static_cast<size_t>(out - buf);
}

}  // namespace ff
