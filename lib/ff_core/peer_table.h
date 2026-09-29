#pragma once
//
// UID-keyed peer table.
//
// Peers are identified by their 32-bit UID (not by a slot number, which no longer
// exists). Position and announce packets update the matching entry, creating one
// on first contact. Entries not refreshed within a timeout are considered lost and
// can be expired. When the table is full, the least-recently-heard peer is evicted.
//
// The active peer count this exposes is the input to ALOHA rate control.
//
// Pure: the current time (millis) is passed in by the caller so the logic is
// deterministic under host tests.
//
#include <cstdint>
#include <cstddef>

#include "protocol.h"

namespace ff {

// Maximum simultaneously-tracked peers. Larger than the legacy fixed 6; the FC's
// radar output (which has its own slot limit) maps the closest/strongest subset.
constexpr size_t kMaxPeers = 16;

// Maximum radios a node runs at once (ESP-NOW + LoRa ...). Defined here because
// both the peer table (per-radio "last heard" tracking) and the Node depend on it.
constexpr size_t kMaxRadios = 4;

struct Peer {
    uint32_t uid = 0;
    bool valid = false;

    // Latest position.
    int32_t lat = 0;            // deg * 1e7
    int32_t lon = 0;            // deg * 1e7
    int16_t alt_m = 0;          // metres MSL
    uint16_t speed_cms = 0;     // cm/s
    uint16_t course_ddeg = 0;   // decidegrees
    uint8_t flags = 0;

    // Identity (from announce).
    char name[kMaxNameLen + 1] = {0};
    uint32_t capabilities = 0;

    // Bookkeeping.
    uint32_t last_update_ms = 0;    // any packet, any radio
    uint32_t last_position_ms = 0;  // position packet specifically
    int16_t rssi = 0;
    uint32_t packets_received = 0;

    // Per-radio "last heard" timestamps and a bit per radio ever heard on. Lets
    // the rate controller count peers per medium: a peer heard only over LoRa
    // does not inflate ESP-NOW's channel load.
    uint32_t last_seen_on[kMaxRadios] = {0};
    uint8_t radios_seen = 0;
};

class PeerTable {
public:
    explicit PeerTable(uint32_t timeout_ms);

    // Update (or create) the peer identified by the packet, recording which radio
    // it was heard on. rssi of 0 means "unknown" and leaves the stored value.
    Peer* updatePosition(const PositionPacket& p, uint32_t now_ms, int16_t rssi,
                         size_t radio_index);
    Peer* updateAnnounce(const AnnouncePacket& a, uint32_t now_ms, size_t radio_index);

    // Invalidate peers not heard within the timeout.
    void expire(uint32_t now_ms);

    // Count of peers heard on any radio within the timeout (excludes expired
    // entries even if expire() has not been called yet).
    uint32_t countActive(uint32_t now_ms) const;

    // Count of peers heard on a specific radio within the timeout. This is what
    // paces that radio's beacon rate.
    uint32_t countActiveOn(size_t radio_index, uint32_t now_ms) const;

    const Peer* find(uint32_t uid) const;

    size_t capacity() const { return kMaxPeers; }
    // Returns the slot if valid, else nullptr. For iteration over [0,capacity()).
    const Peer* at(size_t index) const;

    uint32_t timeoutMs() const { return timeout_ms_; }

private:
    Peer* findMutable(uint32_t uid);
    // Returns a free slot, or the least-recently-updated slot if full.
    Peer* allocSlot();

    Peer peers_[kMaxPeers];
    uint32_t timeout_ms_;
};

}  // namespace ff
