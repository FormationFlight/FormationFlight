#include <unity.h>

#include <cstring>
#include <vector>

#include "node.h"

using namespace ff;

// ---- Fakes ---------------------------------------------------------------

// A configurable set of radios. Defaults to a single radio with 14 ms airtime so
// single-radio tests read naturally.
struct FakeRadioSet : IRadioSet {
    struct Tx {
        uint8_t data[64];
        size_t len;
        size_t radio;
    };
    std::vector<Tx> sent;
    size_t n_radios = 1;
    double airtimes[kMaxRadios] = {14.0, 14.0, 14.0, 14.0};
    bool enabled_[kMaxRadios] = {true, true, true, true};

    size_t radioCount() const override { return n_radios; }
    bool radioEnabled(size_t i) const override { return enabled_[i]; }
    double airtimeMs(size_t i, size_t) const override { return airtimes[i]; }
    void transmit(size_t i, const uint8_t* d, size_t len) override {
        Tx t{};
        std::memcpy(t.data, d, len);
        t.len = len;
        t.radio = i;
        sent.push_back(t);
    }
    size_t countForRadio(size_t i) const {
        size_t c = 0;
        for (auto& t : sent) {
            if (t.radio == i) c++;
        }
        return c;
    }
};

struct FakeLocation : ILocationSource {
    NodeLocation loc;
    NodeLocation getLocation() override { return loc; }
};

struct NullCrypto : ICrypto {
    size_t encrypt(uint8_t*, size_t len, size_t) override { return len; }
    bool decrypt(uint8_t*, size_t len, size_t& out_len) override {
        out_len = len;
        return true;
    }
};

struct RejectCrypto : ICrypto {
    size_t encrypt(uint8_t*, size_t len, size_t) override { return len; }
    bool decrypt(uint8_t*, size_t, size_t&) override { return false; }
};

struct FakeMspRadarSink : IMspRadarSink {
    struct Sent {
        uint8_t slot_id;
        uint32_t peer_uid;  // 0 if the peer had no uid tracked (not used here)
        int32_t lat;
    };
    std::vector<Sent> sent;

    void sendRadarPosition(uint8_t slot_id, const Peer& peer) override {
        sent.push_back({slot_id, peer.uid, peer.lat});
    }
};

static float rngHalf(void*) { return 0.5f; }  // no jitter (factor 1.0)

static size_t makePeerFrame(uint32_t uid, uint8_t* buf, size_t cap,
                            int32_t lat = 100, int32_t lon = 200) {
    PositionPacket p{};
    p.uid = uid;
    p.lat = lat;
    p.lon = lon;
    p.alt_m = 42;
    p.speed_cms = 500;
    p.course_ddeg = 1800;
    p.flags = POSITION_FLAG_HAS_FIX;
    return encodePosition(p, buf, cap);
}

// Inject a peer heard on a specific radio.
static void injectPeer(Node& node, uint32_t uid, uint32_t now, size_t radio) {
    uint8_t buf[64];
    size_t n = makePeerFrame(uid, buf, sizeof(buf));
    node.onReceive(buf, n, now, -50, radio);
}

static NodeConfig baseConfig(uint32_t uid = 1) {
    NodeConfig cfg{};
    cfg.uid = uid;
    std::strcpy(cfg.name, "OWL");
    cfg.capabilities = CAP_HAS_GPS;
    return cfg;
}

void setUp() {}
void tearDown() {}

// ---- Tests ---------------------------------------------------------------

void test_beacons_at_min_rate_when_alone() {
    FakeRadioSet radio;
    FakeLocation location;
    location.loc.valid = true;
    location.loc.lat = 451715460;
    NullCrypto crypto;
    NodeDeps deps{&radio, &location, &crypto, nullptr, rngHalf, nullptr};

    Node node(baseConfig(), deps);
    node.begin(0);
    // Airtime 14, alone -> base 93 -> clamped to 100ms, no jitter -> 100ms.
    for (uint32_t t = 10; t <= 1000; t += 10) {
        node.poll(t);
    }
    // Beacons at 100,200,...,1000 = 10; announce doesn't start until 2000.
    TEST_ASSERT_EQUAL_UINT32(10, radio.sent.size());
    for (auto& tx : radio.sent) {
        PositionPacket p{};
        TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                          static_cast<int>(decodePosition(tx.data, tx.len, p)));
        TEST_ASSERT_EQUAL_UINT32(1, p.uid);
        TEST_ASSERT_TRUE((p.flags & POSITION_FLAG_HAS_FIX) != 0);
    }
    TEST_ASSERT_EQUAL_UINT32(10, node.stats().beacons_sent);
}

void test_beacon_interval_grows_with_peers() {
    FakeRadioSet radio;
    FakeLocation location;
    location.loc.valid = true;
    NullCrypto crypto;
    NodeDeps deps{&radio, &location, &crypto, nullptr, rngHalf, nullptr};

    Node node(baseConfig(), deps);
    for (uint32_t i = 0; i < 5; i++) {
        injectPeer(node, 0x1000 + i, 0, 0);
    }
    TEST_ASSERT_EQUAL_UINT32(5, node.activePeerCountOn(0, 0));

    node.begin(0);
    // 5 peers -> 6 nodes -> base 6*14/0.15 = 560ms. First beacon at ~560.
    node.poll(559);
    TEST_ASSERT_EQUAL_UINT32(0, radio.sent.size());
    node.poll(560);
    TEST_ASSERT_EQUAL_UINT32(1, radio.sent.size());
}

void test_per_radio_rate_keeps_fast_radio_fast() {
    // Fast radio (2 ms) and slow radio (20 ms), both loaded with the same peers.
    FakeRadioSet radio;
    radio.n_radios = 2;
    radio.airtimes[0] = 2.0;
    radio.airtimes[1] = 20.0;
    FakeLocation location;
    location.loc.valid = true;
    NullCrypto crypto;
    NodeDeps deps{&radio, &location, &crypto, nullptr, rngHalf, nullptr};

    Node node(baseConfig(), deps);
    for (uint32_t i = 0; i < 5; i++) {
        injectPeer(node, 0x2000 + i, 0, 0);  // heard on both media
        injectPeer(node, 0x2000 + i, 0, 1);
    }
    node.begin(0);
    for (uint32_t t = 10; t <= 2000; t += 10) {
        node.poll(t);
    }
    // Fast: base 6*2/0.15=80 -> clamp 100ms -> ~20 beacons. Slow: 800ms -> ~2.
    size_t fast = radio.countForRadio(0);
    size_t slow = radio.countForRadio(1);
    TEST_ASSERT_TRUE(fast >= 15);
    TEST_ASSERT_TRUE(slow <= 5);
    TEST_ASSERT_TRUE(fast > slow * 3);
}

void test_peers_on_one_medium_dont_slow_the_other() {
    // The point of per-medium counting: 5 peers heard ONLY on the slow radio must
    // not throttle the fast radio, which sees zero peers on its own medium.
    FakeRadioSet radio;
    radio.n_radios = 2;
    radio.airtimes[0] = 2.0;   // fast (ESP-NOW)
    radio.airtimes[1] = 20.0;  // slow (LoRa)
    FakeLocation location;
    location.loc.valid = true;
    NullCrypto crypto;
    NodeDeps deps{&radio, &location, &crypto, nullptr, rngHalf, nullptr};

    Node node(baseConfig(), deps);
    for (uint32_t i = 0; i < 5; i++) {
        injectPeer(node, 0x3000 + i, 0, 1);  // LoRa only
    }
    TEST_ASSERT_EQUAL_UINT32(5, node.activePeerCountOn(1, 0));
    TEST_ASSERT_EQUAL_UINT32(0, node.activePeerCountOn(0, 0));

    node.begin(0);
    for (uint32_t t = 10; t <= 2000; t += 10) {
        node.poll(t);
    }
    // Fast radio unaffected by the LoRa-only peers -> stays at the 100ms floor.
    TEST_ASSERT_TRUE(radio.countForRadio(0) >= 18);
    TEST_ASSERT_TRUE(radio.countForRadio(1) <= 5);
}

void test_onreceive_populates_peer_table() {
    FakeRadioSet radio;
    NullCrypto crypto;
    NodeDeps deps{&radio, nullptr, &crypto, nullptr, rngHalf, nullptr};
    Node node(baseConfig(), deps);

    uint8_t buf[64];
    size_t n = makePeerFrame(0x55, buf, sizeof(buf), 12345, 67890);
    node.onReceive(buf, n, 1000, -70, 0);

    TEST_ASSERT_EQUAL_UINT32(1, node.stats().rx_ok);
    const Peer* p = node.peers().find(0x55);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_INT32(12345, p->lat);
    TEST_ASSERT_EQUAL_INT32(67890, p->lon);
    TEST_ASSERT_EQUAL_INT16(-70, p->rssi);
}

void test_ignores_own_uid() {
    FakeRadioSet radio;
    NullCrypto crypto;
    NodeDeps deps{&radio, nullptr, &crypto, nullptr, rngHalf, nullptr};
    Node node(baseConfig(7), deps);

    uint8_t buf[64];
    size_t n = makePeerFrame(7, buf, sizeof(buf));
    node.onReceive(buf, n, 1000, -50, 0);

    TEST_ASSERT_EQUAL_UINT32(1, node.stats().rx_self);
    TEST_ASSERT_EQUAL_UINT32(0, node.stats().rx_ok);
    TEST_ASSERT_EQUAL_UINT32(0, node.activePeerCount(1000));
}

void test_listen_only_tracks_but_never_transmits() {
    FakeRadioSet radio;
    FakeLocation location;
    location.loc.valid = true;
    NullCrypto crypto;
    NodeDeps deps{&radio, &location, &crypto, nullptr, rngHalf, nullptr};

    NodeConfig cfg = baseConfig();
    cfg.listen_only = true;
    Node node(cfg, deps);
    node.begin(0);

    injectPeer(node, 0x22, 100, 0);

    for (uint32_t t = 10; t <= 5000; t += 10) {
        node.poll(t);
    }
    TEST_ASSERT_EQUAL_UINT32(0, radio.sent.size());
    TEST_ASSERT_NOT_NULL(node.peers().find(0x22));
}

void test_crypto_rejection_drops_frame() {
    FakeRadioSet radio;
    RejectCrypto crypto;
    NodeDeps deps{&radio, nullptr, &crypto, nullptr, rngHalf, nullptr};
    Node node(baseConfig(), deps);

    uint8_t buf[64];
    size_t n = makePeerFrame(0x99, buf, sizeof(buf));
    node.onReceive(buf, n, 1000, -50, 0);

    TEST_ASSERT_EQUAL_UINT32(1, node.stats().rx_rejected);
    TEST_ASSERT_EQUAL_UINT32(0, node.stats().rx_ok);
    TEST_ASSERT_NULL(node.peers().find(0x99));
}

void test_announce_is_emitted_on_all_radios() {
    FakeRadioSet radio;
    radio.n_radios = 2;
    FakeLocation location;
    location.loc.valid = true;
    NullCrypto crypto;
    NodeDeps deps{&radio, &location, &crypto, nullptr, rngHalf, nullptr};
    Node node(baseConfig(), deps);
    node.begin(0);

    for (uint32_t t = 10; t <= 2100; t += 10) {
        node.poll(t);
    }
    size_t announces[2] = {0, 0};
    for (auto& tx : radio.sent) {
        Header h{};
        if (peekHeader(tx.data, tx.len, h) == DecodeResult::Ok &&
            h.type == PacketType::Announce) {
            AnnouncePacket a{};
            TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                              static_cast<int>(decodeAnnounce(tx.data, tx.len, a)));
            TEST_ASSERT_EQUAL_STRING("OWL", a.name);
            announces[tx.radio]++;
        }
    }
    TEST_ASSERT_TRUE(announces[0] >= 1);
    TEST_ASSERT_TRUE(announces[1] >= 1);
}

void test_disabled_radio_is_not_transmitted_on() {
    FakeRadioSet radio;
    radio.n_radios = 2;
    radio.enabled_[1] = false;  // second radio off
    FakeLocation location;
    location.loc.valid = true;
    NullCrypto crypto;
    NodeDeps deps{&radio, &location, &crypto, nullptr, rngHalf, nullptr};
    Node node(baseConfig(), deps);
    node.begin(0);

    for (uint32_t t = 10; t <= 1000; t += 10) {
        node.poll(t);
    }
    TEST_ASSERT_TRUE(radio.countForRadio(0) > 0);
    TEST_ASSERT_EQUAL_UINT32(0, radio.countForRadio(1));  // disabled, silent
}

// ---- MSP radar output ------------------------------------------------------
//
// These reproduce the reported bug: a node in listen-only (GCS) mode, with no
// radio ever transmitting, must still push its received peers out over the MSP
// radar sink on its own schedule. v1 tied that output to the radio TX cycle, so
// a node that never transmits never sent anything -- the fix is that the sink is
// scheduled independently in Node::begin(), regardless of listen_only or radios.

void test_gcs_mode_still_emits_msp_radar_with_no_radios_and_no_tx() {
    FakeMspRadarSink sink;
    NodeConfig cfg = baseConfig();
    cfg.listen_only = true;            // GCS: never transmits
    cfg.msp_radar_interval_ms = 50;
    // No IRadioSet at all -- the exact "no Tx call in the firmware" scenario.
    NodeDeps deps{nullptr, nullptr, nullptr, &sink, nullptr, nullptr};
    Node node(cfg, deps);

    injectPeer(node, 0xC0FFEE, 0, 0);  // a peer heard over some radio
    node.begin(0);

    for (uint32_t t = 10; t <= 500; t += 10) {
        node.poll(t);
    }
    // Must have received radar sends purely from the sink's own timer.
    TEST_ASSERT_TRUE(sink.sent.size() >= 5);
    for (auto& s : sink.sent) {
        TEST_ASSERT_EQUAL_UINT32(0xC0FFEE, s.peer_uid);
    }
}

void test_msp_radar_no_sink_configured_is_safe() {
    NodeConfig cfg = baseConfig();
    cfg.listen_only = true;
    NodeDeps deps{nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    Node node(cfg, deps);
    injectPeer(node, 0x01, 0, 0);
    node.begin(0);
    for (uint32_t t = 10; t <= 500; t += 10) {
        node.poll(t);  // must not crash with no sink
    }
    TEST_ASSERT_TRUE(true);
}

void test_msp_radar_round_robins_across_peers() {
    FakeMspRadarSink sink;
    NodeConfig cfg = baseConfig();
    cfg.listen_only = true;
    cfg.msp_radar_interval_ms = 50;
    NodeDeps deps{nullptr, nullptr, nullptr, &sink, nullptr, nullptr};
    Node node(cfg, deps);

    injectPeer(node, 0xA1, 0, 0);
    injectPeer(node, 0xA2, 0, 0);
    injectPeer(node, 0xA3, 0, 0);
    node.begin(0);
    for (uint32_t t = 10; t <= 300; t += 10) {
        node.poll(t);
    }
    // All three peers should appear in the sent sequence (round robin), not just
    // the first one repeated.
    bool saw[3] = {false, false, false};
    for (auto& s : sink.sent) {
        if (s.peer_uid == 0xA1) saw[0] = true;
        if (s.peer_uid == 0xA2) saw[1] = true;
        if (s.peer_uid == 0xA3) saw[2] = true;
    }
    TEST_ASSERT_TRUE(saw[0] && saw[1] && saw[2]);
}

void test_msp_radar_active_node_also_gets_output() {
    // Not just GCS mode: a normal transmitting node with an attached FC also
    // needs its peers pushed out over MSP radar, on the same independent timer.
    FakeRadioSet radio;
    FakeLocation location;
    location.loc.valid = true;
    NullCrypto crypto;
    FakeMspRadarSink sink;
    NodeDeps deps{&radio, &location, &crypto, &sink, rngHalf, nullptr};
    NodeConfig cfg = baseConfig();
    cfg.msp_radar_interval_ms = 50;
    Node node(cfg, deps);

    injectPeer(node, 0xB1, 0, 0);
    node.begin(0);
    for (uint32_t t = 10; t <= 300; t += 10) {
        node.poll(t);
    }
    TEST_ASSERT_TRUE(sink.sent.size() >= 4);
}

void test_peer_expires() {
    FakeRadioSet radio;
    FakeLocation location;
    location.loc.valid = true;
    NullCrypto crypto;
    NodeDeps deps{&radio, &location, &crypto, nullptr, rngHalf, nullptr};

    NodeConfig cfg = baseConfig();
    cfg.peer_timeout_ms = 1000;
    Node node(cfg, deps);

    injectPeer(node, 0x33, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(1, node.activePeerCount(0));

    node.begin(0);
    for (uint32_t t = 100; t <= 2000; t += 100) {
        node.poll(t);
    }
    TEST_ASSERT_EQUAL_UINT32(0, node.activePeerCount(2000));
    TEST_ASSERT_NULL(node.peers().find(0x33));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_beacons_at_min_rate_when_alone);
    RUN_TEST(test_beacon_interval_grows_with_peers);
    RUN_TEST(test_per_radio_rate_keeps_fast_radio_fast);
    RUN_TEST(test_peers_on_one_medium_dont_slow_the_other);
    RUN_TEST(test_onreceive_populates_peer_table);
    RUN_TEST(test_ignores_own_uid);
    RUN_TEST(test_listen_only_tracks_but_never_transmits);
    RUN_TEST(test_crypto_rejection_drops_frame);
    RUN_TEST(test_announce_is_emitted_on_all_radios);
    RUN_TEST(test_disabled_radio_is_not_transmitted_on);
    RUN_TEST(test_gcs_mode_still_emits_msp_radar_with_no_radios_and_no_tx);
    RUN_TEST(test_msp_radar_no_sink_configured_is_safe);
    RUN_TEST(test_msp_radar_round_robins_across_peers);
    RUN_TEST(test_msp_radar_active_node_also_gets_output);
    RUN_TEST(test_peer_expires);
    return UNITY_END();
}
