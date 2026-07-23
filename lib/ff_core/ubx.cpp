#include "ubx.h"

#include "wire.h"

namespace ff {

namespace {

int32_t readI32(const uint8_t* p, size_t off) {
    const uint8_t* q = p + off;
    return wire::get_i32(q);
}
uint8_t readU8(const uint8_t* p, size_t off) { return p[off]; }

// Write a full UBX frame (sync..checksum) around a payload already placed at
// buf+6. Returns the total frame length.
size_t finishFrame(uint8_t* buf, uint8_t cls, uint8_t id, uint16_t payload_len) {
    buf[0] = 0xB5;
    buf[1] = 0x62;
    buf[2] = cls;
    buf[3] = id;
    buf[4] = static_cast<uint8_t>(payload_len & 0xFF);
    buf[5] = static_cast<uint8_t>(payload_len >> 8);
    uint8_t ck_a, ck_b;
    ubxChecksum(buf + 2, static_cast<size_t>(4 + payload_len), ck_a, ck_b);
    buf[6 + payload_len] = ck_a;
    buf[6 + payload_len + 1] = ck_b;
    return static_cast<size_t>(8 + payload_len);
}

}  // namespace

void ubxChecksum(const uint8_t* data, size_t len, uint8_t& ck_a, uint8_t& ck_b) {
    uint8_t a = 0, b = 0;
    for (size_t i = 0; i < len; i++) {
        a = static_cast<uint8_t>(a + data[i]);
        b = static_cast<uint8_t>(b + a);
    }
    ck_a = a;
    ck_b = b;
}

void UbxParser::reset() {
    state_ = Sync1;
    idx_ = 0;
    oversize_ = false;
}

bool UbxParser::feed(uint8_t b) {
    switch (state_) {
        case Sync1:
            if (b == 0xB5) state_ = Sync2;
            break;
        case Sync2:
            state_ = (b == 0x62) ? Class : (b == 0xB5 ? Sync2 : Sync1);
            break;
        case Class:
            class_ = b;
            ck_a_ = b;
            ck_b_ = b;
            state_ = Id;
            break;
        case Id:
            id_ = b;
            ck_a_ = static_cast<uint8_t>(ck_a_ + b);
            ck_b_ = static_cast<uint8_t>(ck_b_ + ck_a_);
            state_ = LenLo;
            break;
        case LenLo:
            len_ = b;
            ck_a_ = static_cast<uint8_t>(ck_a_ + b);
            ck_b_ = static_cast<uint8_t>(ck_b_ + ck_a_);
            state_ = LenHi;
            break;
        case LenHi:
            len_ |= static_cast<uint16_t>(b) << 8;
            ck_a_ = static_cast<uint8_t>(ck_a_ + b);
            ck_b_ = static_cast<uint8_t>(ck_b_ + ck_a_);
            idx_ = 0;
            oversize_ = (len_ > kUbxMaxPayload);
            state_ = (len_ == 0) ? CkA : Payload;
            break;
        case Payload:
            ck_a_ = static_cast<uint8_t>(ck_a_ + b);
            ck_b_ = static_cast<uint8_t>(ck_b_ + ck_a_);
            if (!oversize_) payload_[idx_] = b;
            idx_++;
            if (idx_ >= len_) state_ = CkA;
            break;
        case CkA:
            rx_ck_a_ = b;
            state_ = CkB;
            break;
        case CkB: {
            const bool ok = (rx_ck_a_ == ck_a_) && (b == ck_b_) && !oversize_;
            state_ = Sync1;
            return ok;
        }
    }
    return false;
}

bool decodeNavPvt(const uint8_t* p, uint16_t len, UbxFix& out) {
    if (len < 92) {
        return false;
    }
    const uint8_t fix_type = readU8(p, 20);
    const uint8_t flags = readU8(p, 21);
    const bool gnss_ok = (flags & 0x01) != 0;

    out.valid = gnss_ok && (fix_type == 2 || fix_type == 3);
    out.num_sat = readU8(p, 23);
    out.lon = readI32(p, 24);  // deg * 1e7
    out.lat = readI32(p, 28);  // deg * 1e7
    out.alt_m = static_cast<int16_t>(readI32(p, 36) / 1000);   // mm MSL -> m
    out.speed_cms = static_cast<uint16_t>(readI32(p, 60) / 10);  // mm/s -> cm/s

    // headMot is deg * 1e5; convert to decidegrees and normalize to [0,3599].
    int32_t ddeg = readI32(p, 64) / 10000;
    ddeg %= 3600;
    if (ddeg < 0) ddeg += 3600;
    out.course_ddeg = static_cast<uint16_t>(ddeg);
    return true;
}

size_t buildCfgRate(uint16_t meas_ms, uint8_t* buf, size_t cap) {
    const uint16_t payload_len = 6;
    if (cap < 8 + payload_len) return 0;
    uint8_t* pl = buf + 6;
    pl[0] = static_cast<uint8_t>(meas_ms & 0xFF);
    pl[1] = static_cast<uint8_t>(meas_ms >> 8);
    pl[2] = 0x01;  // navRate: 1 measurement per nav solution
    pl[3] = 0x00;
    pl[4] = 0x01;  // timeRef: 1 = GPS time
    pl[5] = 0x00;
    return finishFrame(buf, kUbxClassCfg, kUbxIdCfgRate, payload_len);
}

size_t buildCfgPrtUart(uint32_t baud, uint8_t* buf, size_t cap) {
    const uint16_t payload_len = 20;
    if (cap < 8 + payload_len) return 0;
    uint8_t* pl = buf + 6;
    for (uint16_t i = 0; i < payload_len; i++) pl[i] = 0;
    pl[0] = 0x01;         // portID = UART1
    // pl[1] reserved
    // pl[2..3] txReady = 0
    pl[4] = 0xD0;         // mode = 0x000008D0 (8 data bits, no parity, 1 stop)
    pl[5] = 0x08;
    pl[8] = static_cast<uint8_t>(baud & 0xFF);
    pl[9] = static_cast<uint8_t>((baud >> 8) & 0xFF);
    pl[10] = static_cast<uint8_t>((baud >> 16) & 0xFF);
    pl[11] = static_cast<uint8_t>((baud >> 24) & 0xFF);
    pl[12] = 0x03;        // inProtoMask = UBX + NMEA
    pl[14] = 0x01;        // outProtoMask = UBX only
    return finishFrame(buf, kUbxClassCfg, kUbxIdCfgPrt, payload_len);
}

size_t buildCfgMsg(uint8_t msg_class, uint8_t msg_id, uint8_t rate, uint8_t* buf,
                   size_t cap) {
    const uint16_t payload_len = 3;
    if (cap < 8 + payload_len) return 0;
    uint8_t* pl = buf + 6;
    pl[0] = msg_class;
    pl[1] = msg_id;
    pl[2] = rate;  // per current port: 0 disables, 1 = every nav solution
    return finishFrame(buf, kUbxClassCfg, kUbxIdCfgMsg, payload_len);
}

}  // namespace ff
