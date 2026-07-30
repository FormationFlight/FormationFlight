#include "msp_radar.h"

#include "protocol.h"
#include "wire.h"

namespace ff {

namespace {

constexpr uint16_t kPayloadLen = 19;  // id+state+lat+lon+alt+heading+speed+lq

uint8_t crc8DvbS2(uint8_t crc, uint8_t a) {
    crc ^= a;
    for (int i = 0; i < 8; i++) {
        crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0xD5)
                           : static_cast<uint8_t>(crc << 1);
    }
    return crc;
}

}  // namespace

uint8_t estimateRadarLq(const Peer& peer, uint32_t now_ms, uint32_t timeout_ms) {
    if (timeout_ms == 0) {
        return 0;
    }
    const uint32_t age = now_ms - peer.last_update_ms;
    if (age >= timeout_ms) {
        return 0;
    }
    uint32_t bucket = (age * 4) / timeout_ms;  // 0..3 (age < timeout, so <4)
    if (bucket > 3) {
        bucket = 3;
    }
    return static_cast<uint8_t>(4 - bucket);
}

size_t buildMspSetRadarPos(uint8_t slot_id, const Peer& peer, uint32_t now_ms,
                          uint32_t timeout_ms, uint8_t* buf, size_t cap) {
    if (cap < kMspRadarFrameSize) {
        return 0;
    }

    // Payload, matching msp_radar_pos_t's field order/widths byte-for-byte:
    // id(u8) state(u8) lat(i32) lon(i32) alt(i32) heading(u16) speed(u16) lq(u8)
    uint8_t payload[kPayloadLen];
    uint8_t* p = payload;
    wire::put_u8(p, slot_id);
    wire::put_u8(p, (peer.flags & POSITION_FLAG_ARMED) ? 1 : 0);
    wire::put_i32(p, peer.lat);                                  // deg * 1e7
    wire::put_i32(p, peer.lon);                                  // deg * 1e7
    wire::put_i32(p, static_cast<int32_t>(peer.alt_m) * 100);    // m -> cm
    wire::put_u16(p, static_cast<uint16_t>(peer.course_ddeg / 10));  // ddeg -> deg
    wire::put_u16(p, peer.speed_cms);
    wire::put_u8(p, estimateRadarLq(peer, now_ms, timeout_ms));

    uint8_t* out = buf;
    wire::put_u8(out, '$');
    wire::put_u8(out, 'X');
    wire::put_u8(out, '<');
    wire::put_u8(out, 0);  // flag
    wire::put_u16(out, kMspSetRadarPos);
    wire::put_u16(out, kPayloadLen);

    uint8_t crc = 0;
    crc = crc8DvbS2(crc, 0);  // flag
    crc = crc8DvbS2(crc, static_cast<uint8_t>(kMspSetRadarPos & 0xFF));
    crc = crc8DvbS2(crc, static_cast<uint8_t>(kMspSetRadarPos >> 8));
    crc = crc8DvbS2(crc, static_cast<uint8_t>(kPayloadLen & 0xFF));
    crc = crc8DvbS2(crc, static_cast<uint8_t>(kPayloadLen >> 8));
    for (uint16_t i = 0; i < kPayloadLen; i++) {
        wire::put_u8(out, payload[i]);
        crc = crc8DvbS2(crc, payload[i]);
    }
    wire::put_u8(out, crc);

    return static_cast<size_t>(out - buf);
}

}  // namespace ff
