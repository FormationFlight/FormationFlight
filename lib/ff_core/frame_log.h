#pragma once
//
// A short rolling log of the frames this node has sent and received.
//
// This exists for the web UI's per-radio debug view: when a link is not working
// the question is almost always "is anything arriving at all, and if so why is
// it being thrown away" - a question the aggregate counters cannot answer but a
// list of the last few frames, each with its radio, sender, size, RSSI and
// verdict, answers immediately.
//
// Fixed capacity, no allocation, oldest entry overwritten. Kept deliberately
// small: this sits in RAM on an ESP8285 with ~40 KB of heap.
//
#include <cstddef>
#include <cstdint>

namespace ff {

constexpr size_t kFrameLogCapacity = 32;

// Why a frame is in the log. Ordered so the UI can colour anything above Ok as
// a problem.
enum class FrameResult : uint8_t {
    Tx = 0,           // transmitted by us
    Ok = 1,           // received, authenticated, decoded, folded into the table
    Self = 2,         // our own UID heard back (normal on ESP-NOW broadcast)
    CryptoFail = 3,   // tag did not verify: wrong group passphrase, or noise
    ReplayFail = 4,   // authenticated but the counter did not advance
    DecodeFail = 5,   // authenticated but not a packet we understand
    Oversize = 6,     // longer than the largest frame we accept
};

struct FrameLogEntry {
    uint32_t ms = 0;
    uint32_t uid = 0;      // sender (0 when not yet known)
    uint16_t len = 0;      // bytes on the air
    int16_t rssi = 0;
    uint8_t radio = 0;     // index into the RadioHub
    uint8_t type = 0;      // PacketType, 0 when undecodable
    FrameResult result = FrameResult::Ok;
};

class FrameLog {
public:
    void add(const FrameLogEntry& e) {
        entries_[head_] = e;
        head_ = (head_ + 1) % kFrameLogCapacity;
        if (count_ < kFrameLogCapacity) {
            count_++;
        }
        total_++;
    }

    size_t size() const { return count_; }
    size_t capacity() const { return kFrameLogCapacity; }
    // Total ever logged, so a client can tell it missed some between polls.
    uint32_t total() const { return total_; }

    // Index 0 is the most recent entry.
    const FrameLogEntry& at(size_t i) const {
        const size_t idx = (head_ + kFrameLogCapacity - 1 - (i % kFrameLogCapacity)) %
                           kFrameLogCapacity;
        return entries_[idx];
    }

    void clear() {
        head_ = 0;
        count_ = 0;
    }

private:
    FrameLogEntry entries_[kFrameLogCapacity];
    size_t head_ = 0;
    size_t count_ = 0;
    uint32_t total_ = 0;
};

}  // namespace ff
