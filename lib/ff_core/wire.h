#pragma once
//
// Explicit little-endian (de)serialization helpers.
//
// We serialize field-by-field rather than memcpy'ing packed structs so the wire
// format is fixed regardless of host endianness, struct padding, or compiler.
// This is what lets the same codec run on-device and under native host tests.
//
#include <cstdint>
#include <cstddef>

namespace ff {
namespace wire {

// Writers advance the cursor past the bytes written.
inline void put_u8(uint8_t*& p, uint8_t v) { *p++ = v; }
inline void put_u16(uint8_t*& p, uint16_t v) {
    *p++ = static_cast<uint8_t>(v & 0xFF);
    *p++ = static_cast<uint8_t>((v >> 8) & 0xFF);
}
inline void put_u32(uint8_t*& p, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        *p++ = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    }
}
inline void put_i16(uint8_t*& p, int16_t v) { put_u16(p, static_cast<uint16_t>(v)); }
inline void put_i32(uint8_t*& p, int32_t v) { put_u32(p, static_cast<uint32_t>(v)); }

// Readers advance the cursor past the bytes consumed.
inline uint8_t get_u8(const uint8_t*& p) { return *p++; }
inline uint16_t get_u16(const uint8_t*& p) {
    uint16_t v = static_cast<uint16_t>(p[0]) |
                 static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
    p += 2;
    return v;
}
inline uint32_t get_u32(const uint8_t*& p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        v |= static_cast<uint32_t>(*p++) << (8 * i);
    }
    return v;
}
inline int16_t get_i16(const uint8_t*& p) { return static_cast<int16_t>(get_u16(p)); }
inline int32_t get_i32(const uint8_t*& p) { return static_cast<int32_t>(get_u32(p)); }

}  // namespace wire
}  // namespace ff
