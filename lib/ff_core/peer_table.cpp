#include "peer_table.h"

namespace ff {

PeerTable::PeerTable(uint32_t timeout_ms) : timeout_ms_(timeout_ms) {}

Peer* PeerTable::findMutable(uint32_t uid) {
    for (size_t i = 0; i < kMaxPeers; i++) {
        if (peers_[i].valid && peers_[i].uid == uid) {
            return &peers_[i];
        }
    }
    return nullptr;
}

const Peer* PeerTable::find(uint32_t uid) const {
    for (size_t i = 0; i < kMaxPeers; i++) {
        if (peers_[i].valid && peers_[i].uid == uid) {
            return &peers_[i];
        }
    }
    return nullptr;
}

Peer* PeerTable::allocSlot() {
    Peer* oldest = &peers_[0];
    for (size_t i = 0; i < kMaxPeers; i++) {
        if (!peers_[i].valid) {
            return &peers_[i];
        }
        if (peers_[i].last_update_ms < oldest->last_update_ms) {
            oldest = &peers_[i];
        }
    }
    // Table full: evict the least-recently-updated peer.
    return oldest;
}

namespace {
void markHeardOn(Peer* peer, size_t radio_index, uint32_t now_ms) {
    if (radio_index < kMaxRadios) {
        peer->last_seen_on[radio_index] = now_ms;
        peer->radios_seen |= static_cast<uint8_t>(1u << radio_index);
    }
}
}  // namespace

Peer* PeerTable::updatePosition(const PositionPacket& p, uint32_t now_ms, int16_t rssi,
                                size_t radio_index) {
    Peer* peer = findMutable(p.uid);
    if (peer == nullptr) {
        peer = allocSlot();
        // Reset the (possibly evicted) slot for its new occupant.
        *peer = Peer{};
        peer->uid = p.uid;
        peer->valid = true;
    }
    peer->lat = p.lat;
    peer->lon = p.lon;
    peer->alt_m = p.alt_m;
    peer->speed_cms = p.speed_cms;
    peer->course_ddeg = p.course_ddeg;
    peer->flags = p.flags;
    peer->last_update_ms = now_ms;
    peer->last_position_ms = now_ms;
    if (rssi != 0) {
        peer->rssi = rssi;
    }
    markHeardOn(peer, radio_index, now_ms);
    peer->packets_received++;
    return peer;
}

Peer* PeerTable::updateAnnounce(const AnnouncePacket& a, uint32_t now_ms,
                                size_t radio_index) {
    Peer* peer = findMutable(a.uid);
    if (peer == nullptr) {
        peer = allocSlot();
        *peer = Peer{};
        peer->uid = a.uid;
        peer->valid = true;
    }
    // Copy the name (both buffers are kMaxNameLen+1 and null-terminated).
    size_t i = 0;
    for (; i < kMaxNameLen && a.name[i] != '\0'; i++) {
        peer->name[i] = a.name[i];
    }
    peer->name[i] = '\0';
    peer->capabilities = a.capabilities;
    peer->last_update_ms = now_ms;
    markHeardOn(peer, radio_index, now_ms);
    peer->packets_received++;
    return peer;
}

void PeerTable::expire(uint32_t now_ms) {
    for (size_t i = 0; i < kMaxPeers; i++) {
        if (peers_[i].valid && (now_ms - peers_[i].last_update_ms) > timeout_ms_) {
            peers_[i].valid = false;
        }
    }
}

uint32_t PeerTable::countActive(uint32_t now_ms) const {
    uint32_t n = 0;
    for (size_t i = 0; i < kMaxPeers; i++) {
        if (peers_[i].valid && (now_ms - peers_[i].last_update_ms) <= timeout_ms_) {
            n++;
        }
    }
    return n;
}

uint32_t PeerTable::countActiveOn(size_t radio_index, uint32_t now_ms) const {
    if (radio_index >= kMaxRadios) {
        return 0;
    }
    const uint8_t bit = static_cast<uint8_t>(1u << radio_index);
    uint32_t n = 0;
    for (size_t i = 0; i < kMaxPeers; i++) {
        const Peer& p = peers_[i];
        if (p.valid && (p.radios_seen & bit) &&
            (now_ms - p.last_seen_on[radio_index]) <= timeout_ms_) {
            n++;
        }
    }
    return n;
}

const Peer* PeerTable::at(size_t index) const {
    if (index >= kMaxPeers || !peers_[index].valid) {
        return nullptr;
    }
    return &peers_[index];
}

}  // namespace ff
