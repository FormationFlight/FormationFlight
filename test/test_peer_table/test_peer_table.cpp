#include <unity.h>

#include <cstring>

#include "peer_table.h"

using namespace ff;

void setUp() {}
void tearDown() {}

static PositionPacket makePos(uint32_t uid, int32_t lat = 100, int32_t lon = 200) {
    PositionPacket p{};
    p.uid = uid;
    p.lat = lat;
    p.lon = lon;
    p.alt_m = 50;
    p.speed_cms = 1000;
    p.course_ddeg = 900;
    p.flags = POSITION_FLAG_HAS_FIX;
    return p;
}

void test_create_on_first_position() {
    PeerTable t(6000);
    TEST_ASSERT_EQUAL_UINT32(0, t.countActive(0));
    Peer* p = t.updatePosition(makePos(0xAA), 1000, -42, 0);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT32(0xAA, p->uid);
    TEST_ASSERT_EQUAL_INT16(-42, p->rssi);
    TEST_ASSERT_EQUAL_UINT32(1, p->packets_received);
    TEST_ASSERT_EQUAL_UINT32(1, t.countActive(1000));
}

void test_update_existing_same_slot() {
    PeerTable t(6000);
    t.updatePosition(makePos(0xAA, 100, 200), 1000, -42, 0);
    t.updatePosition(makePos(0xAA, 111, 222), 2000, -40, 0);
    TEST_ASSERT_EQUAL_UINT32(1, t.countActive(2000));
    const Peer* p = t.find(0xAA);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_INT32(111, p->lat);
    TEST_ASSERT_EQUAL_INT32(222, p->lon);
    TEST_ASSERT_EQUAL_INT16(-40, p->rssi);
    TEST_ASSERT_EQUAL_UINT32(2, p->packets_received);
}

void test_rssi_zero_preserves_previous() {
    PeerTable t(6000);
    t.updatePosition(makePos(0xAA), 1000, -55, 0);
    t.updatePosition(makePos(0xAA), 1100, 0, 0);
    TEST_ASSERT_EQUAL_INT16(-55, t.find(0xAA)->rssi);
}

void test_announce_merges_name() {
    PeerTable t(6000);
    t.updatePosition(makePos(0xAB), 1000, -50, 0);
    AnnouncePacket a{};
    a.uid = 0xAB;
    std::strcpy(a.name, "HAWK");
    a.capabilities = CAP_HAS_GPS;
    t.updateAnnounce(a, 1200, 0);
    const Peer* p = t.find(0xAB);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_STRING("HAWK", p->name);
    TEST_ASSERT_EQUAL_UINT32(CAP_HAS_GPS, p->capabilities);
    TEST_ASSERT_EQUAL_INT16(50, p->alt_m);
    TEST_ASSERT_EQUAL_UINT32(1, t.countActive(1200));
}

void test_expiry() {
    PeerTable t(6000);
    t.updatePosition(makePos(0x01), 1000, -50, 0);
    t.updatePosition(makePos(0x02), 2000, -50, 0);
    TEST_ASSERT_EQUAL_UINT32(1, t.countActive(7500));
    t.expire(7500);
    TEST_ASSERT_NULL(t.find(0x01));
    TEST_ASSERT_NOT_NULL(t.find(0x02));
}

void test_count_active_excludes_stale_before_expire() {
    PeerTable t(1000);
    t.updatePosition(makePos(0x01), 0, -50, 0);
    TEST_ASSERT_EQUAL_UINT32(0, t.countActive(5000));
}

void test_full_table_evicts_oldest() {
    PeerTable t(1000000);
    for (uint32_t i = 0; i < kMaxPeers; i++) {
        t.updatePosition(makePos(0x100 + i), (i + 1) * 10, -50, 0);
    }
    TEST_ASSERT_EQUAL_UINT32(kMaxPeers, t.countActive((kMaxPeers + 1) * 10));

    uint32_t now = (kMaxPeers + 2) * 10;
    t.updatePosition(makePos(0x999), now, -50, 0);
    TEST_ASSERT_NULL(t.find(0x100));
    TEST_ASSERT_NOT_NULL(t.find(0x999));
    TEST_ASSERT_NOT_NULL(t.find(0x101));
    TEST_ASSERT_EQUAL_UINT32(kMaxPeers, t.countActive(now));
}

void test_iteration() {
    PeerTable t(6000);
    t.updatePosition(makePos(0x01), 1000, -50, 0);
    t.updatePosition(makePos(0x02), 1000, -50, 0);
    uint32_t seen = 0;
    for (size_t i = 0; i < t.capacity(); i++) {
        const Peer* p = t.at(i);
        if (p != nullptr) {
            seen++;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(2, seen);
}

// ---- Per-radio counting --------------------------------------------------

void test_count_active_on_is_per_radio() {
    PeerTable t(6000);
    t.updatePosition(makePos(0x01), 1000, -50, 0);  // radio 0 only
    t.updatePosition(makePos(0x02), 1000, -50, 1);  // radio 1 only
    t.updatePosition(makePos(0x03), 1000, -50, 0);  // radio 0 and 1
    t.updatePosition(makePos(0x03), 1000, -50, 1);

    TEST_ASSERT_EQUAL_UINT32(3, t.countActive(1000));       // any radio
    TEST_ASSERT_EQUAL_UINT32(2, t.countActiveOn(0, 1000));  // 0x01, 0x03
    TEST_ASSERT_EQUAL_UINT32(2, t.countActiveOn(1, 1000));  // 0x02, 0x03
    TEST_ASSERT_EQUAL_UINT32(0, t.countActiveOn(2, 1000));  // nothing on radio 2
}

void test_count_active_on_respects_per_radio_timeout() {
    PeerTable t(1000);
    // Heard on radio 1 at t=0, then only on radio 0 at t=1500.
    t.updatePosition(makePos(0x07), 0, -50, 1);
    t.updatePosition(makePos(0x07), 1500, -50, 0);
    // At t=2000: radio 1 last-heard 0 (age 2000 > 1000) -> not active on radio 1,
    // radio 0 last-heard 1500 (age 500 <= 1000) -> active on radio 0.
    TEST_ASSERT_EQUAL_UINT32(1, t.countActiveOn(0, 2000));
    TEST_ASSERT_EQUAL_UINT32(0, t.countActiveOn(1, 2000));
    TEST_ASSERT_EQUAL_UINT32(1, t.countActive(2000));  // still active overall
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_create_on_first_position);
    RUN_TEST(test_update_existing_same_slot);
    RUN_TEST(test_rssi_zero_preserves_previous);
    RUN_TEST(test_announce_merges_name);
    RUN_TEST(test_expiry);
    RUN_TEST(test_count_active_excludes_stale_before_expire);
    RUN_TEST(test_full_table_evicts_oldest);
    RUN_TEST(test_iteration);
    RUN_TEST(test_count_active_on_is_per_radio);
    RUN_TEST(test_count_active_on_respects_per_radio_timeout);
    return UNITY_END();
}
