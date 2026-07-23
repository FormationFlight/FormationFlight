#include <unity.h>

#include <cstring>
#include <vector>

#include "protocol.h"
#include "radio_hub.h"

using namespace ff;

// ---- Fakes ---------------------------------------------------------------

struct FakeDriver : RadioDriver {
    std::vector<std::vector<uint8_t>> sent;
    std::vector<RxFrame> inbox;
    size_t read_idx = 0;
    double airtime;
    const char* nm;

    FakeDriver(const char* n, double a) : airtime(a), nm(n) {}

    void transmit(const uint8_t* d, size_t n) override {
        sent.emplace_back(d, d + n);
    }
    double airtimeMs(size_t) const override { return airtime; }
    void serviceRx() override {}
    bool popRx(RxFrame& out) override {
        if (read_idx < inbox.size()) {
            out = inbox[read_idx++];
            return true;
        }
        return false;
    }
    const char* name() const override { return nm; }

    void queueFrame(const uint8_t* d, size_t n, uint32_t ts, int16_t rssi) {
        RxFrame f{};
        std::memcpy(f.data, d, n);
        f.len = static_cast<uint8_t>(n);
        f.timestamp_ms = ts;
        f.rssi = rssi;
        inbox.push_back(f);
    }
};

struct NullCrypto : ICrypto {
    size_t encrypt(uint8_t*, size_t len, size_t) override { return len; }
    bool decrypt(uint8_t*, size_t len, size_t& out_len) override {
        out_len = len;
        return true;
    }
};

static size_t makePeerFrame(uint32_t uid, uint8_t* buf, size_t cap) {
    PositionPacket p{};
    p.uid = uid;
    p.lat = 111;
    p.lon = 222;
    p.flags = POSITION_FLAG_HAS_FIX;
    return encodePosition(p, buf, cap);
}

void setUp() {}
void tearDown() {}

// ---- Tests ---------------------------------------------------------------

void test_add_respects_capacity() {
    RadioHub hub;
    FakeDriver d[kMaxRadios + 1] = {{"a", 1}, {"b", 1}, {"c", 1}, {"d", 1}, {"e", 1}};
    for (size_t i = 0; i < kMaxRadios; i++) {
        TEST_ASSERT_TRUE(hub.add(&d[i]));
    }
    TEST_ASSERT_FALSE(hub.add(&d[kMaxRadios]));  // over capacity
    TEST_ASSERT_EQUAL_UINT32(kMaxRadios, hub.count());
}

void test_transmit_fans_out_to_enabled_only() {
    RadioHub hub;
    FakeDriver espnow("ESPNOW", 2.0);
    FakeDriver lora("SX127x", 20.0);
    hub.add(&espnow);
    hub.add(&lora);

    uint8_t buf[8] = {1, 2, 3, 4, 5};
    hub.transmit(buf, 5);
    TEST_ASSERT_EQUAL_UINT32(1, espnow.sent.size());
    TEST_ASSERT_EQUAL_UINT32(1, lora.sent.size());

    lora.setEnabled(false);
    hub.transmit(buf, 5);
    TEST_ASSERT_EQUAL_UINT32(2, espnow.sent.size());
    TEST_ASSERT_EQUAL_UINT32(1, lora.sent.size());  // unchanged, disabled
}

void test_airtime_is_max_of_enabled() {
    RadioHub hub;
    FakeDriver espnow("ESPNOW", 2.0);
    FakeDriver lora("SX127x", 20.0);
    hub.add(&espnow);
    hub.add(&lora);

    TEST_ASSERT_DOUBLE_WITHIN(0.001, 20.0, hub.airtimeMs(21));  // paces to LoRa
    lora.setEnabled(false);
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 2.0, hub.airtimeMs(21));   // now ESP-NOW
}

void test_service_delivers_from_all_radios_to_node() {
    RadioHub hub;
    FakeDriver espnow("ESPNOW", 2.0);
    FakeDriver lora("SX127x", 20.0);
    hub.add(&espnow);
    hub.add(&lora);

    NullCrypto crypto;
    NodeConfig cfg{};
    cfg.uid = 1;
    NodeDeps deps{&hub, nullptr, &crypto, nullptr, nullptr};
    Node node(cfg, deps);

    // A peer heard on ESP-NOW and a different peer heard on LoRa.
    uint8_t buf[64];
    size_t n1 = makePeerFrame(0xAA, buf, sizeof(buf));
    espnow.queueFrame(buf, n1, 1000, -40);
    size_t n2 = makePeerFrame(0xBB, buf, sizeof(buf));
    lora.queueFrame(buf, n2, 1000, -110);

    hub.service(node);

    TEST_ASSERT_EQUAL_UINT32(2, node.activePeerCount(1000));
    TEST_ASSERT_NOT_NULL(node.peers().find(0xAA));
    TEST_ASSERT_NOT_NULL(node.peers().find(0xBB));
    TEST_ASSERT_EQUAL_INT16(-40, node.peers().find(0xAA)->rssi);
    TEST_ASSERT_EQUAL_INT16(-110, node.peers().find(0xBB)->rssi);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_add_respects_capacity);
    RUN_TEST(test_transmit_fans_out_to_enabled_only);
    RUN_TEST(test_airtime_is_max_of_enabled);
    RUN_TEST(test_service_delivers_from_all_radios_to_node);
    return UNITY_END();
}
