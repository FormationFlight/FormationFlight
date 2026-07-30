#include <unity.h>

#include "msp_radar.h"

using namespace ff;

void setUp() {}
void tearDown() {}

static Peer makePeer(int32_t lat, int32_t lon, int16_t alt_m, uint16_t speed_cms,
                     uint16_t course_ddeg, uint8_t flags, uint32_t last_update_ms) {
    Peer p{};
    p.valid = true;
    p.lat = lat;
    p.lon = lon;
    p.alt_m = alt_m;
    p.speed_cms = speed_cms;
    p.course_ddeg = course_ddeg;
    p.flags = flags;
    p.last_update_ms = last_update_ms;
    return p;
}

// Recompute the DVB-S2 CRC exactly as the MSP wire format defines it, to check
// frames independently of the production implementation.
static uint8_t refCrc8(uint8_t crc, uint8_t a) {
    crc ^= a;
    for (int i = 0; i < 8; i++) {
        crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0xD5)
                           : static_cast<uint8_t>(crc << 1);
    }
    return crc;
}

void test_frame_header_and_size() {
    Peer p = makePeer(451715460, 57223870, 120, 500, 900, POSITION_FLAG_ARMED, 1000);
    uint8_t buf[64];
    size_t n = buildMspSetRadarPos(3, p, 1000, 6000, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(kMspRadarFrameSize, n);
    TEST_ASSERT_EQUAL_UINT8('$', buf[0]);
    TEST_ASSERT_EQUAL_UINT8('X', buf[1]);
    TEST_ASSERT_EQUAL_UINT8('<', buf[2]);
    TEST_ASSERT_EQUAL_UINT8(0, buf[3]);  // flag
    // message id (MSP2_COMMON_SET_RADAR_POS = 0x100B), LE.
    TEST_ASSERT_EQUAL_UINT8(0x0B, buf[4]);
    TEST_ASSERT_EQUAL_UINT8(0x10, buf[5]);
    // payload size = 19, LE.
    TEST_ASSERT_EQUAL_UINT8(19, buf[6]);
    TEST_ASSERT_EQUAL_UINT8(0, buf[7]);
}

void test_frame_payload_fields() {
    Peer p = makePeer(451715460, 57223870, 120, 500, 900, POSITION_FLAG_ARMED, 1000);
    uint8_t buf[64];
    buildMspSetRadarPos(7, p, 1000, 6000, buf, sizeof(buf));
    const uint8_t* pl = buf + 8;
    TEST_ASSERT_EQUAL_UINT8(7, pl[0]);   // slot id
    TEST_ASSERT_EQUAL_UINT8(1, pl[1]);  // armed
    // lat (deg*1e7), LE i32.
    int32_t lat = pl[2] | (pl[3] << 8) | (pl[4] << 16) | (static_cast<int32_t>(pl[5]) << 24);
    TEST_ASSERT_EQUAL_INT32(451715460, lat);
    int32_t lon = pl[6] | (pl[7] << 8) | (pl[8] << 16) | (static_cast<int32_t>(pl[9]) << 24);
    TEST_ASSERT_EQUAL_INT32(57223870, lon);
    // alt: 120 m -> 12000 cm.
    int32_t alt = pl[10] | (pl[11] << 8) | (pl[12] << 16) | (static_cast<int32_t>(pl[13]) << 24);
    TEST_ASSERT_EQUAL_INT32(12000, alt);
    // heading: 900 ddeg -> 90 deg.
    uint16_t heading = pl[14] | (pl[15] << 8);
    TEST_ASSERT_EQUAL_UINT16(90, heading);
    // speed: unchanged, 500 cm/s.
    uint16_t speed = pl[16] | (pl[17] << 8);
    TEST_ASSERT_EQUAL_UINT16(500, speed);
}

void test_disarmed_flag() {
    Peer p = makePeer(0, 0, 0, 0, 0, 0, 1000);  // no ARMED bit
    uint8_t buf[64];
    buildMspSetRadarPos(1, p, 1000, 6000, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT8(0, buf[9]);  // state byte = disarmed
}

void test_checksum_is_valid() {
    Peer p = makePeer(1, 2, 3, 4, 5, POSITION_FLAG_ARMED, 1000);
    uint8_t buf[64];
    size_t n = buildMspSetRadarPos(1, p, 1000, 6000, buf, sizeof(buf));
    uint8_t crc = 0;
    for (size_t i = 3; i < n - 1; i++) {  // flag, id(2), size(2), payload
        crc = refCrc8(crc, buf[i]);
    }
    TEST_ASSERT_EQUAL_UINT8(crc, buf[n - 1]);
}

void test_buffer_too_small_returns_zero() {
    Peer p = makePeer(0, 0, 0, 0, 0, 0, 0);
    uint8_t buf[kMspRadarFrameSize - 1];
    TEST_ASSERT_EQUAL_UINT32(0, buildMspSetRadarPos(1, p, 0, 6000, buf, sizeof(buf)));
}

// ---- LQ estimate ----------------------------------------------------------

void test_lq_fresh_peer_is_max() {
    Peer p = makePeer(0, 0, 0, 0, 0, 0, 1000);
    TEST_ASSERT_EQUAL_UINT8(4, estimateRadarLq(p, 1000, 6000));  // age 0
}

void test_lq_decreases_with_age() {
    Peer p = makePeer(0, 0, 0, 0, 0, 0, 0);
    uint8_t lq_early = estimateRadarLq(p, 500, 6000);   // age 500 / 6000
    uint8_t lq_mid = estimateRadarLq(p, 3000, 6000);    // age 3000 / 6000
    uint8_t lq_late = estimateRadarLq(p, 5900, 6000);   // age 5900 / 6000
    TEST_ASSERT_TRUE(lq_early >= lq_mid);
    TEST_ASSERT_TRUE(lq_mid >= lq_late);
}

void test_lq_zero_once_timed_out() {
    Peer p = makePeer(0, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(0, estimateRadarLq(p, 6000, 6000));   // age == timeout
    TEST_ASSERT_EQUAL_UINT8(0, estimateRadarLq(p, 9000, 6000));   // well past
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_frame_header_and_size);
    RUN_TEST(test_frame_payload_fields);
    RUN_TEST(test_disarmed_flag);
    RUN_TEST(test_checksum_is_valid);
    RUN_TEST(test_buffer_too_small_returns_zero);
    RUN_TEST(test_lq_fresh_peer_is_max);
    RUN_TEST(test_lq_decreases_with_age);
    RUN_TEST(test_lq_zero_once_timed_out);
    return UNITY_END();
}
