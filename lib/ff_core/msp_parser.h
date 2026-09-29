#pragma once
//
// Non-blocking MSP response parser (v1 and v2 framing).
//
// The v1 firmware read MSP by busy-waiting on the serial port up to a timeout,
// inside the main loop -- one of the blocking calls the rearchitecture set out to
// remove. This parser instead consumes bytes one at a time as they become
// available and signals when a complete, checksum-valid frame has arrived, so the
// caller only ever drains the bytes already buffered and never waits.
//
// MSP v1 response framing:  '$' 'M' '>' [size] [id] [payload x size] [crc]
//   where crc = size ^ id ^ (each payload byte).
// MSP v2 response framing:  '$' 'X' '>' [flag] [id lo] [id hi] [size lo] [size hi]
//                           [payload x size] [crc]
//   where crc = CRC-8/DVB-S2 over flag..payload.
//
// Both are needed: INAV answers most telemetry over v1 ids, but the mixer
// (platform type) and the full-width status bitmask only exist as v2 messages.
//
// Pure and host-testable: it knows nothing about serial ports or message
// semantics -- it just turns a byte stream into framed (id, payload) units.
//
#include <cstdint>

namespace ff {

// Largest payload retained. Sized for MSP_RC (32 bytes) and an INAV MSP_BOXIDS
// reply (one byte per enabled box, typically 30-50) with headroom.
constexpr uint16_t kMspMaxPayload = 128;

class MspParser {
public:
    // Feed one received byte. Returns true exactly when that byte completes a
    // valid frame; version(), id(), size(), flag() and payload() are then
    // readable until the next feed() call.
    bool feed(uint8_t b);

    uint8_t version() const { return version_; }  // 1 or 2
    uint16_t id() const { return id_; }
    uint16_t size() const { return size_; }
    uint8_t flag() const { return flag_; }        // v2 only; 0 for v1
    const uint8_t* payload() const { return payload_; }

    // Return to the idle state (e.g. after a link reset).
    void reset();

private:
    enum State : uint8_t {
        Idle,
        HdrM,
        V1Dir,
        V1Size,
        V1Id,
        V1Payload,
        V1Crc,
        V2Dir,
        V2Flag,
        V2IdLo,
        V2IdHi,
        V2SizeLo,
        V2SizeHi,
        V2Payload,
        V2Crc,
    };

    State state_ = Idle;
    uint8_t version_ = 0;
    uint16_t size_ = 0;
    uint16_t id_ = 0;
    uint8_t flag_ = 0;
    uint16_t idx_ = 0;
    uint8_t crc_ = 0;
    bool oversize_ = false;
    uint8_t payload_[kMspMaxPayload] = {0};
};

}  // namespace ff
