#include "SimRadio.h"

#include <Arduino.h>
#include <cstring>

namespace ff {

namespace {
constexpr size_t kQueueSize = 8;
}

void SimRadio::begin(CcmCrypto* crypto, uint32_t beacon_interval_ms) {
    crypto_ = crypto;
    interval_ms_ = beacon_interval_ms == 0 ? 200 : beacon_interval_ms;
    clear();
}

void SimRadio::clear() {
    count_ = 0;
    head_ = 0;
    tail_ = 0;
}

bool SimRadio::setPeer(const SimPeerConfig& cfg, uint32_t now_ms) {
    if (cfg.uid == 0) {
        return false;
    }
    for (size_t i = 0; i < count_; i++) {
        if (peers_[i].cfg.uid == cfg.uid) {
            // Replacing a peer restarts its path, so an edit in the UI produces
            // a visible change rather than a jump mid-orbit.
            peers_[i].cfg = cfg;
            peers_[i].start_ms = now_ms;
            peers_[i].announced = false;
            return true;
        }
    }
    if (count_ >= kMaxSimPeers) {
        return false;
    }
    SimPeer& p = peers_[count_++];
    p.cfg = cfg;
    p.start_ms = now_ms;
    p.next_tx_ms = now_ms;
    p.counter = 0;
    p.announced = false;
    return true;
}

bool SimRadio::removePeer(uint32_t uid) {
    for (size_t i = 0; i < count_; i++) {
        if (peers_[i].cfg.uid == uid) {
            for (size_t j = i; j + 1 < count_; j++) {
                peers_[j] = peers_[j + 1];
            }
            count_--;
            return true;
        }
    }
    return false;
}

void SimRadio::emit(SimPeer& peer, bool announce, uint32_t now_ms) {
    const size_t next_head = (head_ + 1) % kQueueSize;
    if (next_head == tail_) {
        return;  // queue full; the loop will drain it next iteration
    }

    uint8_t buf[64];
    size_t len = 0;
    if (announce) {
        AnnouncePacket a{};
        a.uid = peer.cfg.uid;
        std::strncpy(a.name, peer.cfg.name, kMaxNameLen);
        a.name[kMaxNameLen] = '\0';
        a.capabilities = CAP_HAS_GPS;
        len = encodeAnnounce(a, buf, sizeof(buf));
    } else {
        const SimPeerState state = simPeerAt(peer.cfg, now_ms - peer.start_ms);
        const PositionPacket p = simPositionPacket(peer.cfg, state);
        len = encodePosition(p, buf, sizeof(buf));
    }
    if (len == 0) {
        return;
    }

    if (crypto_ != nullptr) {
        peer.counter++;
        len = crypto_->encryptAs(peer.cfg.uid, peer.counter, buf, len, sizeof(buf));
        if (len == 0) {
            return;
        }
    }

    RxFrame& f = queue_[head_];
    f.timestamp_ms = now_ms;
    // A plausible, slightly varying level so the UI's signal columns are not a
    // wall of identical numbers. Not meant to model path loss.
    f.rssi = static_cast<int16_t>(-60 - static_cast<int16_t>((peer.cfg.uid + peer.counter) % 25));
    f.len = static_cast<uint8_t>(len);
    std::memcpy(f.data, buf, len);
    head_ = next_head;
}

void SimRadio::service(uint32_t now_ms) {
    for (size_t i = 0; i < count_; i++) {
        SimPeer& p = peers_[i];
        if (static_cast<int32_t>(now_ms - p.next_tx_ms) < 0) {
            continue;
        }
        p.next_tx_ms = now_ms + interval_ms_;
        // One announce first, so the peer arrives with a name rather than
        // appearing anonymously until the next announce interval.
        if (!p.announced) {
            p.announced = true;
            emit(p, /*announce=*/true, now_ms);
        }
        emit(p, /*announce=*/false, now_ms);
    }
}

void SimRadio::serviceRx() {
    // Frames are produced by service(), which the main loop calls with the
    // current time; there is no hardware to poll here.
}

bool SimRadio::popRx(RxFrame& out) {
    if (tail_ == head_) {
        return false;
    }
    out = queue_[tail_];
    tail_ = (tail_ + 1) % kQueueSize;
    return true;
}

}  // namespace ff
