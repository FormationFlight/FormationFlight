#pragma once
//
// One frame held back while the radio finishes the previous one.
//
// A LoRa radio is half-duplex and a single frame occupies the channel for tens
// of milliseconds: 42.6 ms for a position frame at SF8 / 500 kHz / CR 4/7. The
// node has two independent transmit schedules on that one radio -- the
// per-radio ALOHA beacon, paced by the rate controller, and the node announce,
// which runs on its own fixed timer and fans out to every radio -- and neither
// knows the other exists. On the 915 settings they land on top of each other
// roughly once every six seconds.
//
// Without somewhere to put the second frame a driver has only two options, and
// both are bad: abandon the frame already on the air, which puts a truncated
// packet on the channel for every receiver in range to fail a CRC on, or throw
// the new one away. The second is what the drivers used to do, which is why a
// healthy bench node reported a steadily climbing transmit-drop count.
//
// One slot deep, deliberately. The realistic worst case is exactly two frames
// wanting the radio at once, because the beacon interval is never shorter than
// one airtime. A deeper queue would only add latency to frames whose entire
// value is being current -- a position report delivered three airtimes late is
// worse than no position report, because the receiver cannot tell it is stale.
// A third frame arriving inside one airtime is a real overrun, and is still
// counted as a drop.
//
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ff {

class TxSlot {
public:
    // Matches RxFrame's payload capacity: the largest thing either direction
    // carries is the same frame.
    static constexpr size_t kCapacity = 64;

    // Holds a frame for the driver to send when the radio goes idle.
    //
    // Returns false when it could not be held, which the caller should count as
    // a genuine drop: either the slot is still occupied, or the frame is too
    // long for it. Never overwrites a held frame -- the one already waiting is
    // the older of the two, so overwriting would drop it silently *and* deliver
    // the newer one out of order.
    bool push(const uint8_t* data, size_t len) {
        if (full_ || data == nullptr || len == 0 || len > kCapacity) {
            return false;
        }
        std::memcpy(buf_, data, len);
        len_ = static_cast<uint8_t>(len);
        full_ = true;
        deferred_++;
        return true;
    }

    bool empty() const { return !full_; }
    const uint8_t* data() const { return buf_; }
    size_t size() const { return len_; }
    void clear() {
        full_ = false;
        len_ = 0;
    }

    // Frames deferred rather than dropped, since boot.
    //
    // Kept apart from the drop count on purpose. A deferral costs one airtime
    // of latency and nothing else; a drop is a transmission that never
    // happened and that no counter anywhere else will record. Showing them as
    // one number is what made a working radio look broken.
    uint32_t deferred() const { return deferred_; }

private:
    uint8_t buf_[kCapacity] = {0};
    uint8_t len_ = 0;
    bool full_ = false;
    uint32_t deferred_ = 0;
};

}  // namespace ff
