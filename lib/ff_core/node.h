#pragma once
//
// Node: the v2 application core.
//
// This is the replacement for v1's phase machine (MODE_START -> HOST_SCAN ->
// OTA_SCAN -> OTA_SYNC -> OTA_RX -> OTA_TX) and its web of `sys`/`curr`/`cfg`
// globals. A Node:
//
//   - listens continuously (no scan/sync phases: it is live at boot),
//   - beacons its position on an ALOHA schedule that slows down as the peer
//     count grows (RateController),
//   - folds received beacons/announces into a UID-keyed PeerTable,
//   - ignores its own UID instead of the old, broken "heard my slot -> pick a
//     new slot" dance.
//
// Everything the Node touches that involves hardware is behind an interface
// (radio, location source, crypto, RNG), so the whole application state machine
// is exercised on the host with fakes. The firmware provides real implementations
// in a thin adapter layer. There are no globals and no cooperative megaloop: the
// loop() just calls poll(millis()).
//
#include <cstddef>
#include <cstdint>

#include "peer_table.h"
#include "protocol.h"
#include "rate_control.h"
#include "scheduler.h"

namespace ff {

// A position fix from the node's own location source (FC over MSP, or a directly
// attached GPS). Units match the wire protocol to keep the Node free of
// conversions.
struct NodeLocation {
    bool valid = false;         // true once we have a usable fix
    bool armed = false;         // FC arm state, if known
    int32_t lat = 0;            // deg * 1e7
    int32_t lon = 0;            // deg * 1e7
    int16_t alt_m = 0;          // metres MSL
    uint16_t speed_cms = 0;     // cm/s
    uint16_t course_ddeg = 0;   // decidegrees 0..3599
};

// Maximum radios the Node paces independently and the RadioHub aggregates.
constexpr size_t kMaxRadios = 4;

// The set of radios the Node transmits through. Each radio is addressed by index
// so the Node can pace and transmit on them independently -- ESP-NOW stays fast
// while LoRa slows down as peers grow, because each radio's beacon interval is
// sized from its own airtime. Receive is push-driven: the hardware layer drains
// each radio's RX ring and calls Node::onReceive(). RadioHub implements this.
class IRadioSet {
public:
    virtual ~IRadioSet() = default;
    virtual size_t radioCount() const = 0;
    virtual bool radioEnabled(size_t index) const = 0;
    // Time on air for a payload on radio `index`, used to size that radio's rate.
    virtual double airtimeMs(size_t index, size_t payload_len) const = 0;
    // Transmit on a single radio.
    virtual void transmit(size_t index, const uint8_t* data, size_t len) = 0;
};

// Source of the node's own position.
class ILocationSource {
public:
    virtual ~ILocationSource() = default;
    virtual NodeLocation getLocation() = 0;
};

// Frame confidentiality/integrity. Transforms happen in place; encrypt may grow
// the buffer (an AEAD tag in v2's Phase 2), decrypt may shrink it and can reject
// a frame (bad MIC / wrong group).
class ICrypto {
public:
    virtual ~ICrypto() = default;
    // Returns the ciphertext length, or 0 on failure (e.g. capacity too small).
    virtual size_t encrypt(uint8_t* buf, size_t len, size_t cap) = 0;
    // Returns true on success and writes the plaintext length to out_len.
    virtual bool decrypt(uint8_t* buf, size_t len, size_t& out_len) = 0;
};

// Uniform random in [0,1), injected so jitter is deterministic under test.
using RandomFn = float (*)(void* ctx);

struct NodeConfig {
    uint32_t uid = 0;
    char name[kMaxNameLen + 1] = {0};
    uint32_t capabilities = 0;
    RateConfig rate;
    uint32_t peer_timeout_ms = 6000;
    uint32_t announce_interval_ms = 2000;
    uint32_t expire_interval_ms = 1000;
    // GCS / silent mode: track peers but never transmit.
    bool listen_only = false;
};

struct NodeDeps {
    IRadioSet* radios = nullptr;
    ILocationSource* location = nullptr;
    ICrypto* crypto = nullptr;
    RandomFn rng = nullptr;
    void* rng_ctx = nullptr;
};

struct NodeStats {
    uint32_t beacons_sent = 0;
    uint32_t announces_sent = 0;
    uint32_t rx_ok = 0;
    uint32_t rx_rejected = 0;   // decrypt/decode/validation failures
    uint32_t rx_self = 0;       // our own UID, ignored
    uint32_t last_tx_ms = 0;
};

class Node {
public:
    Node(const NodeConfig& cfg, const NodeDeps& deps);

    // Registers the periodic work (beacon, announce, peer expiry) and primes
    // the rate controller from the radio's airtime. Pass the current time base.
    void begin(uint32_t now_ms);

    // Drive the scheduler. Call every loop iteration with millis(). Returns the
    // milliseconds until the next scheduled work is due.
    uint32_t poll(uint32_t now_ms);

    // Feed a raw received frame from the hardware RX path.
    void onReceive(const uint8_t* data, size_t len, uint32_t now_ms, int16_t rssi);

    const PeerTable& peers() const { return peers_; }
    const NodeStats& stats() const { return stats_; }
    uint32_t activePeerCount(uint32_t now_ms) const { return peers_.countActive(now_ms); }

private:
    // Per-radio beacon context passed to the scheduler trampoline.
    struct BeaconSlot {
        Node* node = nullptr;
        uint8_t index = 0;
    };

    static void beaconTrampoline(void* ctx);
    static void announceTrampoline(void* ctx);
    static void expireTrampoline(void* ctx);

    void onBeaconTick(size_t index);
    void sendBeacon(size_t index);
    void sendAnnounce();
    uint32_t nextBeaconDelayMs(size_t index);

    NodeConfig cfg_;
    NodeDeps deps_;
    Scheduler sched_;
    PeerTable peers_;
    RateController rate_;  // shared config/clamps; airtime supplied per radio
    NodeStats stats_;

    size_t radio_count_ = 0;
    double airtime_[kMaxRadios] = {0};
    BeaconSlot beacon_slots_[kMaxRadios];
    TimerHandle beacon_timer_[kMaxRadios];
    TimerHandle announce_timer_ = kInvalidTimer;
    TimerHandle expire_timer_ = kInvalidTimer;
};

}  // namespace ff
