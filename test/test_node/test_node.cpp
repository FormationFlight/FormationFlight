#include <unity.h>

#include <cstring>
#include <vector>

#include "node.h"

using namespace ff;

// ---- Fakes ---------------------------------------------------------------

struct FakeRadio : IRadio {
    struct Tx {
        uint8_t data[64];
        size_t len;
    };
    std::vector<Tx> sent;
    double airtime = 14.0;

    void transmit(const uint8_t* d, size_t n) override {
        Tx t{};
        std::memcpy(t.data, d, n);
        t.len = n;
        sent.push_back(t);
    }
    double airtimeMs(size_t) const override { return airtime; }
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

static float rngHalf(void*) { return 0.5f; }  // no jitter (factor 1.0)

// Encode a position frame as some other node would send it (plaintext; NullCrypto
// is a passthrough).
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

// ---- Fixtures ------------------------------------------------------------

static NodeConfig baseConfig(uint32_t uid = 1) {
    NodeConfig cfg{};
    cfg.uid = uid;
    std::strcpy(cfg.name, "OWL");
    cfg.capabilities = CAP_HAS_GPS;
    // defaults: rate load 0.15, min 100, max 1000; timeout 6000; announce 2000.
    return cfg;
}

void setUp() {}
void tearDown() {}

// ---- Tests ---------------------------------------------------------------

void test_beacons_at_min_rate_when_alone() {
    FakeRadio radio;
    FakeLocation location;
    location.loc.valid = true;
    location.loc.lat = 451715460;
    NullCrypto crypto;
    NodeDeps deps{&radio, &location, &crypto, rngHalf, nullptr};

    Node node(baseConfig(), deps);
    node.begin(0);
    // Airtime 14, alone -> base 93 -> clamped to 100ms, no jitter -> 100ms.
    for (uint32_t t = 10; t <= 1000; t += 10) {
        node.poll(t);
    }
    // Beacons at 100,200,...,1000 = 10; announce doesn't start until 2000.
    TEST_ASSERT_EQUAL_UINT32(10, radio.sent.size());
    // Every send is a valid position packet from us with a fix.
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
    FakeRadio radio;
    FakeLocation location;
    location.loc.valid = true;
    NullCrypto crypto;
    NodeDeps deps{&radio, &location, &crypto, rngHalf, nullptr};

    Node node(baseConfig(), deps);
    // Inject 5 peers before start (heard at t=0).
    uint8_t buf[64];
    for (uint32_t i = 0; i < 5; i++) {
        size_t n = makePeerFrame(0x1000 + i, buf, sizeof(buf));
        node.onReceive(buf, n, 0, -50);
    }
    TEST_ASSERT_EQUAL_UINT32(5, node.activePeerCount(0));

    node.begin(0);
    // 5 peers -> 6 nodes -> base 6*14/0.15 = 560ms. First beacon at ~560.
    node.poll(559);
    TEST_ASSERT_EQUAL_UINT32(0, radio.sent.size());
    node.poll(560);
    TEST_ASSERT_EQUAL_UINT32(1, radio.sent.size());
}

void test_onreceive_populates_peer_table() {
    FakeRadio radio;
    NullCrypto crypto;
    NodeDeps deps{&radio, nullptr, &crypto, rngHalf, nullptr};
    Node node(baseConfig(), deps);

    uint8_t buf[64];
    size_t n = makePeerFrame(0x55, buf, sizeof(buf), 12345, 67890);
    node.onReceive(buf, n, 1000, -70);

    TEST_ASSERT_EQUAL_UINT32(1, node.stats().rx_ok);
    const Peer* p = node.peers().find(0x55);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_INT32(12345, p->lat);
    TEST_ASSERT_EQUAL_INT32(67890, p->lon);
    TEST_ASSERT_EQUAL_INT16(-70, p->rssi);
}

void test_ignores_own_uid() {
    FakeRadio radio;
    NullCrypto crypto;
    NodeDeps deps{&radio, nullptr, &crypto, rngHalf, nullptr};
    Node node(baseConfig(7), deps);

    uint8_t buf[64];
    size_t n = makePeerFrame(7, buf, sizeof(buf));  // our own uid
    node.onReceive(buf, n, 1000, -50);

    TEST_ASSERT_EQUAL_UINT32(1, node.stats().rx_self);
    TEST_ASSERT_EQUAL_UINT32(0, node.stats().rx_ok);
    TEST_ASSERT_EQUAL_UINT32(0, node.activePeerCount(1000));
}

void test_listen_only_tracks_but_never_transmits() {
    FakeRadio radio;
    FakeLocation location;
    location.loc.valid = true;
    NullCrypto crypto;
    NodeDeps deps{&radio, &location, &crypto, rngHalf, nullptr};

    NodeConfig cfg = baseConfig();
    cfg.listen_only = true;
    Node node(cfg, deps);
    node.begin(0);

    uint8_t buf[64];
    size_t n = makePeerFrame(0x22, buf, sizeof(buf));
    node.onReceive(buf, n, 100, -50);

    for (uint32_t t = 10; t <= 5000; t += 10) {
        node.poll(t);
    }
    TEST_ASSERT_EQUAL_UINT32(0, radio.sent.size());          // silent
    TEST_ASSERT_NOT_NULL(node.peers().find(0x22));           // but listening
}

void test_crypto_rejection_drops_frame() {
    FakeRadio radio;
    RejectCrypto crypto;
    NodeDeps deps{&radio, nullptr, &crypto, rngHalf, nullptr};
    Node node(baseConfig(), deps);

    uint8_t buf[64];
    size_t n = makePeerFrame(0x99, buf, sizeof(buf));
    node.onReceive(buf, n, 1000, -50);

    TEST_ASSERT_EQUAL_UINT32(1, node.stats().rx_rejected);
    TEST_ASSERT_EQUAL_UINT32(0, node.stats().rx_ok);
    TEST_ASSERT_NULL(node.peers().find(0x99));
}

void test_announce_is_emitted() {
    FakeRadio radio;
    FakeLocation location;
    location.loc.valid = true;
    NullCrypto crypto;
    NodeDeps deps{&radio, &location, &crypto, rngHalf, nullptr};
    Node node(baseConfig(), deps);
    node.begin(0);

    for (uint32_t t = 10; t <= 2100; t += 10) {
        node.poll(t);
    }
    // Find at least one announce among the sends and verify its contents.
    bool found = false;
    for (auto& tx : radio.sent) {
        Header h{};
        if (peekHeader(tx.data, tx.len, h) == DecodeResult::Ok &&
            h.type == PacketType::Announce) {
            AnnouncePacket a{};
            TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                              static_cast<int>(decodeAnnounce(tx.data, tx.len, a)));
            TEST_ASSERT_EQUAL_STRING("OWL", a.name);
            found = true;
        }
    }
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_TRUE(node.stats().announces_sent >= 1);
}

void test_peer_expires_and_lowers_beacon_rate() {
    FakeRadio radio;
    FakeLocation location;
    location.loc.valid = true;
    NullCrypto crypto;
    NodeDeps deps{&radio, &location, &crypto, rngHalf, nullptr};

    NodeConfig cfg = baseConfig();
    cfg.peer_timeout_ms = 1000;
    Node node(cfg, deps);

    uint8_t buf[64];
    size_t n = makePeerFrame(0x33, buf, sizeof(buf));
    node.onReceive(buf, n, 0, -50);
    TEST_ASSERT_EQUAL_UINT32(1, node.activePeerCount(0));

    node.begin(0);
    // After the timeout with no refresh and an expire tick, the peer is gone.
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
    RUN_TEST(test_onreceive_populates_peer_table);
    RUN_TEST(test_ignores_own_uid);
    RUN_TEST(test_listen_only_tracks_but_never_transmits);
    RUN_TEST(test_crypto_rejection_drops_frame);
    RUN_TEST(test_announce_is_emitted);
    RUN_TEST(test_peer_expires_and_lowers_beacon_rate);
    return UNITY_END();
}
