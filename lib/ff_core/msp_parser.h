#pragma once
//
// Non-blocking MSP v1 response parser.
//
// The v1 firmware read MSP by busy-waiting on the serial port up to a timeout,
// inside the main loop -- one of the blocking calls the rearchitecture set out to
// remove. This parser instead consumes bytes one at a time as they become
// available and signals when a complete, CRC-valid frame has arrived, so the
// caller only ever drains the bytes already buffered and never waits.
//
// MSP v1 response framing:  '$' 'M' '>' [size] [id] [payload x size] [crc]
// where crc = size ^ id ^ (each payload byte).
//
// Pure and host-testable: it knows nothing about serial ports or message
// semantics -- it just turns a byte stream into framed (id, payload) units.
//
#include <cstdint>

namespace ff {

constexpr uint8_t kMspMaxPayload = 64;

class MspParser {
public:
    // Feed one received byte. Returns true exactly when that byte completes a
    // valid frame; id(), size() and payload() are then readable until the next
    // feed() call.
    bool feed(uint8_t b);

    uint8_t id() const { return id_; }
    uint8_t size() const { return size_; }
    const uint8_t* payload() const { return payload_; }

    // Return to the idle state (e.g. after a link reset).
    void reset();

private:
    enum State : uint8_t { Idle, HdrM, HdrDir, Size, Id, Payload, Crc };

    State state_ = Idle;
    uint8_t size_ = 0;
    uint8_t id_ = 0;
    uint8_t idx_ = 0;
    uint8_t crc_ = 0;
    bool oversize_ = false;
    uint8_t payload_[kMspMaxPayload] = {0};
};

}  // namespace ff
