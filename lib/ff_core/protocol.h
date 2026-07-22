#pragma once
//
// FormationFlight v2 wire protocol (plaintext layer).
//
// This module defines the over-the-air packet formats and their (de)serialization.
// It is deliberately free of any crypto or hardware concerns: encryption wraps the
// byte buffers this produces, and the radio drivers move those bytes. Keeping it
// pure makes the wire format exhaustively unit-testable on the host.
//
// Identity is a 32-bit UID (derived on-device from the efuse MAC). There are no
// slot IDs -- channel access is randomized (ALOHA), so a packet only ever needs to
// say who it is from, never which timeslot it "owns".
//
// All multi-byte fields are little-endian (see wire.h).
//
#include <cstdint>
#include <cstddef>

namespace ff {

// Bump on any incompatible wire change. v1 (the legacy iNav-Radar-derived format)
// is intentionally not interoperable.
constexpr uint8_t kProtocolVersion = 2;

enum class PacketType : uint8_t {
    Position = 1,  // frequent position beacon
    Announce = 2,  // infrequent identity/capability broadcast
};

// Common 6-byte header prefixing every packet.
//   [0]    version
//   [1]    type
//   [2..5] uid (u32 LE)
constexpr size_t kHeaderSize = 6;

// Position beacon: header + 15-byte payload = 21 bytes.
//   lat/lon      : degrees * 1e7 (int32), matches GPS/MSP resolution
//   alt_m        : altitude MSL in metres (int16)
//   speed_cms    : ground speed in cm/s (uint16)
//   course_ddeg  : ground course in decidegrees, 0..3599 (uint16)
//   flags        : status bits (see PositionFlags)
constexpr size_t kPositionPayloadSize = 15;
constexpr size_t kPositionPacketSize = kHeaderSize + kPositionPayloadSize;  // 21

enum PositionFlags : uint8_t {
    POSITION_FLAG_ARMED = 1 << 0,
    POSITION_FLAG_HAS_FIX = 1 << 1,
    // bits 2..7 reserved
};

// Craft name capacity (bytes, excluding the null terminator).
constexpr size_t kMaxNameLen = 15;

// Announce: header + 1-byte name length + name bytes + 4-byte capabilities.
// Variable length; kAnnounceMaxSize bounds the buffer.
constexpr size_t kAnnounceMaxSize = kHeaderSize + 1 + kMaxNameLen + 4;  // 26

enum Capabilities : uint32_t {
    CAP_HAS_GPS = 1 << 0,
    CAP_HAS_MSP_FC = 1 << 1,
    // bits 2..31 reserved
};

struct Header {
    uint8_t version;
    PacketType type;
    uint32_t uid;
};

struct PositionPacket {
    uint32_t uid;
    int32_t lat;              // deg * 1e7
    int32_t lon;              // deg * 1e7
    int16_t alt_m;            // metres MSL
    uint16_t speed_cms;       // cm/s
    uint16_t course_ddeg;     // decidegrees 0..3599
    uint8_t flags;
};

struct AnnouncePacket {
    uint32_t uid;
    char name[kMaxNameLen + 1];  // null-terminated
    uint32_t capabilities;
};

enum class DecodeResult {
    Ok,
    TooShort,        // buffer smaller than the declared/expected packet
    BadVersion,      // protocol version mismatch
    BadType,         // unknown/unsupported packet type
    LengthMismatch,  // internal length field inconsistent with buffer
};

// Encoders return the number of bytes written, or 0 if buf_len is too small.
size_t encodePosition(const PositionPacket& p, uint8_t* buf, size_t buf_len);
size_t encodeAnnounce(const AnnouncePacket& a, uint8_t* buf, size_t buf_len);

// Reads just the common header (cheap triage before a full decode).
DecodeResult peekHeader(const uint8_t* buf, size_t len, Header& out);

DecodeResult decodePosition(const uint8_t* buf, size_t len, PositionPacket& out);
DecodeResult decodeAnnounce(const uint8_t* buf, size_t len, AnnouncePacket& out);

}  // namespace ff
