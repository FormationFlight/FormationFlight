#include <unity.h>

#include "airtime.h"
#include "protocol.h"
#include "rate_control.h"

using namespace ff;

void setUp() {}
void tearDown() {}

// ---- Airtime -------------------------------------------------------------

void test_airtime_matches_known_reference() {
    // Cross-check the formula against a well-known external value: The Things
    // Network airtime calculator reports ~56.6 ms for SF7/BW125k/CR4-5, explicit
    // header, CRC on, 8-symbol preamble, 20-byte payload.
    LoraParams p{};
    p.spreading_factor = 7;
    p.bandwidth_hz = 125000;
    p.coding_rate_denom = 5;
    p.preamble_symbols = 8;
    p.explicit_header = true;
    p.crc_on = true;
    p.low_data_rate_optimize = false;

    double ms = loraAirtimeMs(p, 20);
    TEST_ASSERT_DOUBLE_WITHIN(0.5, 56.6, ms);
}

void test_airtime_default_mode() {
    // FormationFlight fast mode: SF6, 250 kHz, 4/8, implicit header (SF6 requires
    // it), 21-byte position packet. Hand-computed reference ~19.5 ms.
    LoraParams p{};
    p.spreading_factor = 6;
    p.bandwidth_hz = 250000;
    p.coding_rate_denom = 8;
    p.preamble_symbols = 8;
    p.explicit_header = false;
    p.crc_on = true;
    p.low_data_rate_optimize = false;

    double ms = loraAirtimeMs(p, kPositionPacketSize);
    TEST_ASSERT_DOUBLE_WITHIN(0.5, 19.5, ms);
}

void test_airtime_faster_bandwidth_is_shorter() {
    LoraParams p{};
    p.spreading_factor = 7;
    p.bandwidth_hz = 125000;
    p.coding_rate_denom = 5;
    p.preamble_symbols = 8;
    p.explicit_header = true;
    p.crc_on = true;
    p.low_data_rate_optimize = false;

    double at_125 = loraAirtimeMs(p, 20);
    p.bandwidth_hz = 500000;
    double at_500 = loraAirtimeMs(p, 20);
    TEST_ASSERT_TRUE(at_500 < at_125);
}

void test_airtime_grows_with_spreading_factor() {
    LoraParams p{};
    p.spreading_factor = 7;
    p.bandwidth_hz = 250000;
    p.coding_rate_denom = 5;
    p.preamble_symbols = 8;
    p.explicit_header = true;
    p.crc_on = true;
    p.low_data_rate_optimize = false;

    double at_sf7 = loraAirtimeMs(p, 21);
    p.spreading_factor = 10;
    double at_sf10 = loraAirtimeMs(p, 21);
    TEST_ASSERT_TRUE(at_sf10 > at_sf7);
}

// ---- Rate control --------------------------------------------------------

void test_alone_clamps_to_min_interval() {
    RateConfig cfg{};  // defaults: load 0.15, min 100, max 1000
    RateController rc(cfg);
    rc.setAirtimeMs(14.0);
    // Alone: base = 1 * 14 / 0.15 = 93ms -> clamped up to min 100ms.
    TEST_ASSERT_EQUAL_UINT32(100, rc.baseIntervalMs(0));
}

void test_interval_increases_with_peers() {
    RateConfig cfg{};
    RateController rc(cfg);
    rc.setAirtimeMs(14.0);
    uint32_t few = rc.baseIntervalMs(2);
    uint32_t many = rc.baseIntervalMs(20);
    TEST_ASSERT_TRUE(many > few);
}

void test_interval_holds_target_load_in_midrange() {
    RateConfig cfg{};
    RateController rc(cfg);
    rc.setAirtimeMs(14.0);
    // 5 peers -> 6 nodes total. base = 6 * 14 / 0.15 = 560ms (within bounds).
    uint32_t interval = rc.baseIntervalMs(5);
    TEST_ASSERT_UINT32_WITHIN(2, 560, interval);
    // Verify aggregate load ~ target.
    double load = (6.0 * rc.airtimeMs()) / static_cast<double>(interval);
    TEST_ASSERT_FLOAT_WITHIN(0.02, cfg.target_load, static_cast<float>(load));
}

void test_crowded_clamps_to_max_interval() {
    RateConfig cfg{};
    RateController rc(cfg);
    rc.setAirtimeMs(8.0);
    // 100 peers would want ~5.4s; clamp to max 1000ms.
    TEST_ASSERT_EQUAL_UINT32(1000, rc.baseIntervalMs(100));
}

void test_jitter_bounds() {
    RateConfig cfg{};
    cfg.jitter_frac = 0.25f;
    RateController rc(cfg);
    rc.setAirtimeMs(14.0);
    uint32_t base = rc.baseIntervalMs(5);  // 560ms

    // rand01 = 0.0 -> factor 0.75 (minimum)
    uint32_t lo = rc.nextDelayMs(5, 0.0f);
    // rand01 -> 1.0 (approached) -> factor ~1.25 (maximum)
    uint32_t hi = rc.nextDelayMs(5, 0.9999f);
    // rand01 = 0.5 -> factor 1.0 (no jitter)
    uint32_t mid = rc.nextDelayMs(5, 0.5f);

    TEST_ASSERT_UINT32_WITHIN(2, static_cast<uint32_t>(base * 0.75), lo);
    TEST_ASSERT_UINT32_WITHIN(2, static_cast<uint32_t>(base * 1.25), hi);
    TEST_ASSERT_EQUAL_UINT32(base, mid);
    TEST_ASSERT_TRUE(lo < mid);
    TEST_ASSERT_TRUE(mid < hi);
}

void test_airtime_parameterized_paces_each_medium() {
    // One controller, shared config, different airtimes -> different intervals.
    // This is how the Node keeps ESP-NOW fast while LoRa slows under load.
    RateConfig cfg{};
    RateController rc(cfg);
    // 5 peers -> 6 nodes total.
    uint32_t fast = rc.baseIntervalMs(5, 2.0);   // ESP-NOW: 6*2/0.15=80 -> clamp 100
    uint32_t slow = rc.baseIntervalMs(5, 20.0);  // LoRa: 6*20/0.15=800
    TEST_ASSERT_EQUAL_UINT32(100, fast);
    TEST_ASSERT_UINT32_WITHIN(2, 800, slow);
    TEST_ASSERT_TRUE(slow > fast);
}

void test_airtime_parameterized_jitter() {
    RateConfig cfg{};
    cfg.jitter_frac = 0.25f;
    RateController rc(cfg);
    uint32_t base = rc.baseIntervalMs(5, 20.0);  // 800
    TEST_ASSERT_EQUAL_UINT32(base, rc.nextDelayMs(5, 0.5f, 20.0));           // mid
    TEST_ASSERT_UINT32_WITHIN(2, base * 3 / 4, rc.nextDelayMs(5, 0.0f, 20.0));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_airtime_matches_known_reference);
    RUN_TEST(test_airtime_parameterized_paces_each_medium);
    RUN_TEST(test_airtime_parameterized_jitter);
    RUN_TEST(test_airtime_default_mode);
    RUN_TEST(test_airtime_faster_bandwidth_is_shorter);
    RUN_TEST(test_airtime_grows_with_spreading_factor);
    RUN_TEST(test_alone_clamps_to_min_interval);
    RUN_TEST(test_interval_increases_with_peers);
    RUN_TEST(test_interval_holds_target_load_in_midrange);
    RUN_TEST(test_crowded_clamps_to_max_interval);
    RUN_TEST(test_jitter_bounds);
    return UNITY_END();
}
