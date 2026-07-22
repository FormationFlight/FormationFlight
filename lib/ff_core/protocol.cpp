#include "protocol.h"

#include "wire.h"

namespace ff {

namespace {

void writeHeader(uint8_t*& p, PacketType type, uint32_t uid) {
    wire::put_u8(p, kProtocolVersion);
    wire::put_u8(p, static_cast<uint8_t>(type));
    wire::put_u32(p, uid);
}

// Length of a null-terminated string, capped at max_len. Avoids depending on
// strnlen, which is not reliably present in embedded C libraries.
size_t boundedLen(const char* s, size_t max_len) {
    size_t n = 0;
    while (n < max_len && s[n] != '\0') {
        n++;
    }
    return n;
}

}  // namespace

size_t encodePosition(const PositionPacket& p, uint8_t* buf, size_t buf_len) {
    if (buf_len < kPositionPacketSize) {
        return 0;
    }
    uint8_t* cursor = buf;
    writeHeader(cursor, PacketType::Position, p.uid);
    wire::put_i32(cursor, p.lat);
    wire::put_i32(cursor, p.lon);
    wire::put_i16(cursor, p.alt_m);
    wire::put_u16(cursor, p.speed_cms);
    wire::put_u16(cursor, p.course_ddeg);
    wire::put_u8(cursor, p.flags);
    return static_cast<size_t>(cursor - buf);
}

size_t encodeAnnounce(const AnnouncePacket& a, uint8_t* buf, size_t buf_len) {
    size_t name_len = boundedLen(a.name, kMaxNameLen);
    size_t total = kHeaderSize + 1 + name_len + 4;
    if (buf_len < total) {
        return 0;
    }
    uint8_t* cursor = buf;
    writeHeader(cursor, PacketType::Announce, a.uid);
    wire::put_u8(cursor, static_cast<uint8_t>(name_len));
    for (size_t i = 0; i < name_len; i++) {
        wire::put_u8(cursor, static_cast<uint8_t>(a.name[i]));
    }
    wire::put_u32(cursor, a.capabilities);
    return static_cast<size_t>(cursor - buf);
}

DecodeResult peekHeader(const uint8_t* buf, size_t len, Header& out) {
    if (len < kHeaderSize) {
        return DecodeResult::TooShort;
    }
    const uint8_t* cursor = buf;
    out.version = wire::get_u8(cursor);
    uint8_t type = wire::get_u8(cursor);
    out.uid = wire::get_u32(cursor);
    if (out.version != kProtocolVersion) {
        return DecodeResult::BadVersion;
    }
    if (type != static_cast<uint8_t>(PacketType::Position) &&
        type != static_cast<uint8_t>(PacketType::Announce)) {
        return DecodeResult::BadType;
    }
    out.type = static_cast<PacketType>(type);
    return DecodeResult::Ok;
}

DecodeResult decodePosition(const uint8_t* buf, size_t len, PositionPacket& out) {
    Header hdr;
    DecodeResult r = peekHeader(buf, len, hdr);
    if (r != DecodeResult::Ok) {
        return r;
    }
    if (hdr.type != PacketType::Position) {
        return DecodeResult::BadType;
    }
    if (len < kPositionPacketSize) {
        return DecodeResult::TooShort;
    }
    const uint8_t* cursor = buf + kHeaderSize;
    out.uid = hdr.uid;
    out.lat = wire::get_i32(cursor);
    out.lon = wire::get_i32(cursor);
    out.alt_m = wire::get_i16(cursor);
    out.speed_cms = wire::get_u16(cursor);
    out.course_ddeg = wire::get_u16(cursor);
    out.flags = wire::get_u8(cursor);
    return DecodeResult::Ok;
}

DecodeResult decodeAnnounce(const uint8_t* buf, size_t len, AnnouncePacket& out) {
    Header hdr;
    DecodeResult r = peekHeader(buf, len, hdr);
    if (r != DecodeResult::Ok) {
        return r;
    }
    if (hdr.type != PacketType::Announce) {
        return DecodeResult::BadType;
    }
    // Need at least header + name_len byte.
    if (len < kHeaderSize + 1) {
        return DecodeResult::TooShort;
    }
    const uint8_t* cursor = buf + kHeaderSize;
    uint8_t name_len = wire::get_u8(cursor);
    if (name_len > kMaxNameLen) {
        return DecodeResult::LengthMismatch;
    }
    // Need name bytes + 4-byte capabilities.
    if (len < kHeaderSize + 1 + name_len + 4) {
        return DecodeResult::TooShort;
    }
    out.uid = hdr.uid;
    for (size_t i = 0; i < name_len; i++) {
        out.name[i] = static_cast<char>(wire::get_u8(cursor));
    }
    out.name[name_len] = '\0';
    out.capabilities = wire::get_u32(cursor);
    return DecodeResult::Ok;
}

}  // namespace ff
