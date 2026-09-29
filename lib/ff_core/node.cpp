#include "node.h"

namespace ff {

namespace {
// Upper bound for a single frame, plaintext packet plus AEAD expansion.
constexpr size_t kMaxRxFrame = 64;
static_assert(kMaxRxFrame >= kAnnounceFrameSize, "RX buffer too small for an announce frame");
}  // namespace

Node::Node(const NodeConfig& cfg, const NodeDeps& deps)
    : cfg_(cfg), deps_(deps), peers_(cfg.peer_timeout_ms), rate_(cfg.rate) {}

void Node::begin(uint32_t now_ms) {
    // Prime the scheduler's clock so timers are scheduled relative to real time.
    sched_.poll(now_ms);

    radio_count_ = 0;
    if (deps_.radios != nullptr) {
        radio_count_ = deps_.radios->radioCount();
        if (radio_count_ > kMaxRadios) {
            radio_count_ = kMaxRadios;
        }
    }
    for (size_t i = 0; i < kMaxRadios; i++) {
        beacon_timer_[i] = kInvalidTimer;
    }

    // Each radio beacons on its own schedule, paced by its own airtime, so a slow
    // medium (LoRa) backing off does not throttle a fast one (ESP-NOW).
    for (size_t i = 0; i < radio_count_; i++) {
        // Size the rate from the frame that actually goes on the air, crypto
        // overhead included -- the pre-crypto packet size would under-estimate
        // channel occupancy by a third.
        airtime_[i] = deps_.radios->airtimeMs(i, kPositionFrameSize);
        radio_stats_[i].airtime_ms = airtime_[i];
        beacon_slots_[i].node = this;
        beacon_slots_[i].index = static_cast<uint8_t>(i);
        // A listen-only node (e.g. a ground station) tracks peers but never emits.
        if (!cfg_.listen_only) {
            beacon_timer_[i] =
                sched_.after(nextBeaconDelayMs(i), beaconTrampoline, &beacon_slots_[i]);
        }
    }

    if (!cfg_.listen_only) {
        announce_timer_ =
            sched_.every(cfg_.announce_interval_ms, announceTrampoline, this);
    }
    expire_timer_ = sched_.every(cfg_.expire_interval_ms, expireTrampoline, this);

    // Deliberately unconditional: not gated by listen_only, radio_count_, or any
    // transmit activity. See IMspRadarSink's doc comment for why that matters.
    if (deps_.msp_radar_sink != nullptr) {
        msp_radar_cursor_ = 0;
        msp_radar_timer_ =
            sched_.every(cfg_.msp_radar_interval_ms, mspRadarTrampoline, this);
    }
}

uint32_t Node::poll(uint32_t now_ms) { return sched_.poll(now_ms); }

uint32_t Node::nextBeaconDelayMs(size_t index) {
    // Only peers actually heard on this radio count toward its channel load.
    const uint32_t active = peers_.countActiveOn(index, sched_.now());
    const float r = (deps_.rng != nullptr) ? deps_.rng(deps_.rng_ctx) : 0.5f;
    return rate_.nextDelayMs(active, r, airtime_[index]);
}

void Node::beaconTrampoline(void* ctx) {
    BeaconSlot* slot = static_cast<BeaconSlot*>(ctx);
    slot->node->onBeaconTick(slot->index);
}

void Node::onBeaconTick(size_t index) {
    sendBeacon(index);
    // Re-arm this radio's beacon with fresh jitter and a peer-count-adjusted
    // interval sized from this radio's airtime.
    const uint32_t delay = nextBeaconDelayMs(index);
    radio_stats_[index].beacon_interval_ms = delay;
    sched_.rearm(beacon_timer_[index], delay);
}

void Node::announceTrampoline(void* ctx) {
    static_cast<Node*>(ctx)->sendAnnounce();
}

void Node::expireTrampoline(void* ctx) {
    Node* self = static_cast<Node*>(ctx);
    self->peers_.expire(self->sched_.now());
}

void Node::mspRadarTrampoline(void* ctx) {
    static_cast<Node*>(ctx)->onMspRadarTick();
}

void Node::onMspRadarTick() {
    if (deps_.msp_radar_sink == nullptr) {
        return;
    }
    // Round-robin across the peer table, sending the first valid peer found and
    // leaving the cursor just past it for next tick. Scans the whole table at
    // most once so a sparsely-populated table doesn't loop forever.
    const size_t cap = peers_.capacity();
    for (size_t tries = 0; tries < cap; tries++) {
        const size_t idx = msp_radar_cursor_;
        msp_radar_cursor_ = (msp_radar_cursor_ + 1) % cap;
        const Peer* peer = peers_.at(idx);
        if (peer != nullptr) {
            // slot_id is derived from table position (1-based), not the peer's
            // UID: MSP radar consumers expect small stable-ish integer IDs.
            deps_.msp_radar_sink->sendRadarPosition(static_cast<uint8_t>(idx + 1),
                                                     *peer);
            return;
        }
    }
    // No peers currently known; nothing to send this tick.
}

void Node::sendBeacon(size_t index) {
    if (deps_.radios == nullptr || !deps_.radios->radioTransmits(index)) {
        return;
    }
    NodeLocation loc =
        (deps_.location != nullptr) ? deps_.location->getLocation() : NodeLocation{};

    PositionPacket pkt{};
    pkt.uid = cfg_.uid;
    pkt.lat = loc.lat;
    pkt.lon = loc.lon;
    pkt.alt_m = loc.alt_m;
    pkt.speed_cms = loc.speed_cms;
    pkt.course_ddeg = loc.course_ddeg;
    pkt.flags = 0;
    if (loc.valid) {
        pkt.flags |= POSITION_FLAG_HAS_FIX;
    }
    if (loc.armed) {
        pkt.flags |= POSITION_FLAG_ARMED;
    }

    uint8_t buf[kMaxRxFrame];
    size_t len = encodePosition(pkt, buf, sizeof(buf));
    if (len == 0) {
        return;
    }
    if (deps_.crypto != nullptr) {
        len = deps_.crypto->encrypt(buf, len, sizeof(buf));
        if (len == 0) {
            return;
        }
    }
    deps_.radios->transmit(index, buf, len);
    stats_.beacons_sent++;
    stats_.last_tx_ms = sched_.now();
    radio_stats_[index].tx++;
    logFrame(index, cfg_.uid, len, 0, static_cast<uint8_t>(PacketType::Position),
             FrameResult::Tx);
}

void Node::sendAnnounce() {
    if (deps_.radios == nullptr) {
        return;
    }
    AnnouncePacket pkt{};
    pkt.uid = cfg_.uid;
    for (size_t i = 0; i < kMaxNameLen && cfg_.name[i] != '\0'; i++) {
        pkt.name[i] = cfg_.name[i];
    }
    pkt.capabilities = cfg_.capabilities;

    uint8_t plain[kMaxRxFrame];
    const size_t plain_len = encodeAnnounce(pkt, plain, sizeof(plain));
    if (plain_len == 0) {
        return;
    }

    // Announce (identity, not rate-controlled) goes out on every enabled radio,
    // encrypted SEPARATELY for each. Encrypting once and broadcasting the same
    // bytes would put the same frame counter on both media, and the receiver's
    // replay check would correctly reject the second copy as a duplicate -- so
    // announces would only ever land on whichever radio happened to arrive
    // first, and the peer would never be marked as heard on the other one.
    bool sent = false;
    for (size_t i = 0; i < radio_count_; i++) {
        if (!deps_.radios->radioTransmits(i)) {
            continue;
        }
        uint8_t buf[kMaxRxFrame];
        for (size_t b = 0; b < plain_len; b++) {
            buf[b] = plain[b];
        }
        size_t len = plain_len;
        if (deps_.crypto != nullptr) {
            len = deps_.crypto->encrypt(buf, len, sizeof(buf));
            if (len == 0) {
                continue;
            }
        }
        deps_.radios->transmit(i, buf, len);
        radio_stats_[i].tx++;
        logFrame(i, cfg_.uid, len, 0, static_cast<uint8_t>(PacketType::Announce),
                 FrameResult::Tx);
        sent = true;
    }
    if (!sent) {
        return;
    }
    stats_.announces_sent++;
    stats_.last_tx_ms = sched_.now();
}

void Node::logFrame(size_t radio_index, uint32_t uid, size_t len, int16_t rssi, uint8_t type,
                    FrameResult result) {
    FrameLogEntry e;
    e.ms = sched_.now();
    e.uid = uid;
    e.len = static_cast<uint16_t>(len);
    e.rssi = rssi;
    e.radio = static_cast<uint8_t>(radio_index);
    e.type = type;
    e.result = result;
    frames_.add(e);
}

void Node::onReceive(const uint8_t* data, size_t len, uint32_t now_ms, int16_t rssi,
                     size_t radio_index) {
    RadioStats& rs = radio_stats_[radio_index < kMaxRadios ? radio_index : 0];
    rs.last_rx_ms = now_ms;
    rs.last_rssi = rssi;

    if (len == 0 || len > kMaxRxFrame) {
        stats_.rx_rejected++;
        rs.rx_decode_fail++;
        logFrame(radio_index, 0, len, rssi, 0, FrameResult::Oversize);
        return;
    }
    uint8_t buf[kMaxRxFrame];
    for (size_t i = 0; i < len; i++) {
        buf[i] = data[i];
    }

    size_t plain_len = len;
    if (deps_.crypto != nullptr) {
        if (!deps_.crypto->decrypt(buf, len, plain_len, now_ms)) {
            stats_.rx_rejected++;
            const bool replay = deps_.crypto->lastRejectWasReplay();
            if (replay) {
                rs.rx_replay++;
            } else {
                rs.rx_crypto_fail++;
            }
            logFrame(radio_index, 0, len, rssi, 0,
                     replay ? FrameResult::ReplayFail : FrameResult::CryptoFail);
            return;
        }
    }

    Header hdr;
    if (peekHeader(buf, plain_len, hdr) != DecodeResult::Ok) {
        stats_.rx_rejected++;
        rs.rx_decode_fail++;
        logFrame(radio_index, 0, len, rssi, 0, FrameResult::DecodeFail);
        return;
    }

    // Our own UID: ignore. (v1 reacted to hearing "its slot" by reassigning
    // slots; with UID identity there is nothing to do.)
    if (hdr.uid == cfg_.uid) {
        stats_.rx_self++;
        rs.rx_self++;
        logFrame(radio_index, hdr.uid, len, rssi, static_cast<uint8_t>(hdr.type),
                 FrameResult::Self);
        return;
    }

    switch (hdr.type) {
        case PacketType::Position: {
            PositionPacket pkt;
            if (decodePosition(buf, plain_len, pkt) != DecodeResult::Ok) {
                stats_.rx_rejected++;
                rs.rx_decode_fail++;
                logFrame(radio_index, hdr.uid, len, rssi, static_cast<uint8_t>(hdr.type),
                         FrameResult::DecodeFail);
                return;
            }
            peers_.updatePosition(pkt, now_ms, rssi, radio_index);
            stats_.rx_ok++;
            rs.rx_ok++;
            logFrame(radio_index, hdr.uid, len, rssi, static_cast<uint8_t>(hdr.type),
                     FrameResult::Ok);
            break;
        }
        case PacketType::Announce: {
            AnnouncePacket pkt;
            if (decodeAnnounce(buf, plain_len, pkt) != DecodeResult::Ok) {
                stats_.rx_rejected++;
                rs.rx_decode_fail++;
                logFrame(radio_index, hdr.uid, len, rssi, static_cast<uint8_t>(hdr.type),
                         FrameResult::DecodeFail);
                return;
            }
            peers_.updateAnnounce(pkt, now_ms, radio_index);
            stats_.rx_ok++;
            rs.rx_ok++;
            logFrame(radio_index, hdr.uid, len, rssi, static_cast<uint8_t>(hdr.type),
                     FrameResult::Ok);
            break;
        }
        default:
            stats_.rx_rejected++;
            rs.rx_decode_fail++;
            logFrame(radio_index, hdr.uid, len, rssi, 0, FrameResult::DecodeFail);
            return;
    }
}

}  // namespace ff
