#pragma once
#include "crypto.h"
#include "radio_hub.h"
#include "rx_frame.h"
#include "sim_traffic.h"

namespace ff {

// A virtual radio that manufactures peers.
//
// It is a RadioDriver like any other, so the Node addresses it by index, counts
// it in the peer table, and feeds its frames through the identical path real RF
// takes: the same protocol decode, the same AES-CCM authentication, the same
// replay check. That is the whole design intent. A simulator that injected
// Peer structs directly into the table would prove nothing about the receive
// path, and the receive path is where the interesting bugs live.
//
// Transmissions into it are discarded: nothing is listening, and pretending
// otherwise (echoing our own beacons back) would give a lone node a phantom
// peer. It reports a tiny airtime so it never slows the real radios' rate
// control when the Node sizes their beacon intervals.
//
// The simulator is off unless the config says otherwise, and everything that
// can see it -- the status API, the web UI banner -- says loudly when it is on.
// Mistaking a simulated aircraft for a real one is the one failure mode of this
// feature that actually matters.
class SimRadio : public RadioDriver {
public:
    static constexpr size_t kMaxSimPeers = 4;

    // `crypto` may be null, in which case frames are injected as plaintext (the
    // bench passthrough mode). Otherwise frames are encrypted as though they
    // came from the simulated peer, with the real group key.
    void begin(CcmCrypto* crypto, uint32_t beacon_interval_ms = 200);

    // RadioDriver
    void transmit(const uint8_t* /*data*/, size_t /*len*/) override {}
    double airtimeMs(size_t /*payload_len*/) const override { return 0.1; }
    void serviceRx() override;
    bool popRx(RxFrame& out) override;
    const char* name() const override { return "SIM"; }

    // Call every loop with millis(); frames are produced on the beacon schedule.
    void service(uint32_t now_ms);

    // Adds or replaces a simulated peer. Returns false when full or when the
    // config is unusable (zero uid). The path starts at the moment of the call.
    bool setPeer(const SimPeerConfig& cfg, uint32_t now_ms);
    bool removePeer(uint32_t uid);
    void clear();

    size_t peerCount() const { return count_; }
    const SimPeerConfig* peerAt(size_t i) const { return i < count_ ? &peers_[i].cfg : nullptr; }
    // Elapsed milliseconds on peer i's path, for the status view.
    uint32_t peerElapsedMs(size_t i, uint32_t now_ms) const {
        return i < count_ ? (now_ms - peers_[i].start_ms) : 0;
    }

private:
    struct SimPeer {
        SimPeerConfig cfg;
        uint32_t start_ms = 0;
        uint32_t next_tx_ms = 0;
        uint32_t counter = 0;   // its own replay counter, as a real node would have
        bool announced = false;
    };

    // Builds and queues one frame from `peer`, position or announce.
    void emit(SimPeer& peer, bool announce, uint32_t now_ms);

    CcmCrypto* crypto_ = nullptr;
    uint32_t interval_ms_ = 200;
    SimPeer peers_[kMaxSimPeers];
    size_t count_ = 0;

    // Small queue: frames are generated in service() and drained by the hub in
    // the same loop iteration, so it never needs to be deep.
    RxFrame queue_[8];
    size_t head_ = 0;
    size_t tail_ = 0;
};

}  // namespace ff
