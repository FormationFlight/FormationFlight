#include <unity.h>

#include <cstring>
#include <vector>

#include "ubx.h"

using namespace ff;

void setUp() {}
void tearDown() {}

static void putI32(uint8_t* p, size_t off, int32_t v) {
    p[off] = v & 0xFF;
    p[off + 1] = (v >> 8) & 0xFF;
    p[off + 2] = (v >> 16) & 0xFF;
    p[off + 3] = (v >> 24) & 0xFF;
}

static std::vector<uint8_t> makeFrame(uint8_t cls, uint8_t id,
                                      const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> f = {0xB5, 0x62, cls, id,
                              static_cast<uint8_t>(payload.size() & 0xFF),
                              static_cast<uint8_t>(payload.size() >> 8)};
    f.insert(f.end(), payload.begin(), payload.end());
    uint8_t a, b;
    ubxChecksum(f.data() + 2, 4 + payload.size(), a, b);
    f.push_back(a);
    f.push_back(b);
    return f;
}

static std::vector<uint8_t> navPvtPayload() {
    std::vector<uint8_t> p(92, 0);
    p[20] = 3;     // fixType = 3D
    p[21] = 0x01;  // flags: gnssFixOK
    p[23] = 9;     // numSV
    putI32(p.data(), 24, 57223870);   // lon 5.722387 deg
    putI32(p.data(), 28, 451715460);  // lat 45.171546 deg
    putI32(p.data(), 36, 120000);     // hMSL 120 m (mm)
    putI32(p.data(), 60, 5000);       // gSpeed 500 cm/s (mm/s)
    putI32(p.data(), 64, 9000000);    // headMot 90.0 deg (1e-5)
    return p;
}

// ---- Config builders -----------------------------------------------------

void test_cfg_rate_matches_known_10hz_command() {
    uint8_t buf[32];
    size_t n = buildCfgRate(100, buf, sizeof(buf));  // 100 ms = 10 Hz
    // The canonical u-blox "set 10 Hz" command.
    const uint8_t expected[] = {0xB5, 0x62, 0x06, 0x08, 0x06, 0x00, 0x64,
                                0x00, 0x01, 0x00, 0x01, 0x00, 0x7A, 0x12};
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, buf, sizeof(expected));
}

void test_cfg_msg_enables_nav_pvt() {
    uint8_t buf[32];
    size_t n = buildCfgMsg(kUbxClassNav, kUbxIdNavPvt, 1, buf, sizeof(buf));
    const uint8_t expected[] = {0xB5, 0x62, 0x06, 0x01, 0x03, 0x00,
                                0x01, 0x07, 0x01, 0x13, 0x51};
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, buf, sizeof(expected));
}

void test_cfg_prt_sets_baud_and_ubx_out() {
    uint8_t buf[32];
    size_t n = buildCfgPrtUart(115200, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(28, n);  // 8 + 20
    // Baud 115200 = 0x0001C200 at payload offset 8 (buf offset 14), LE.
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[14]);
    TEST_ASSERT_EQUAL_UINT8(0xC2, buf[15]);
    TEST_ASSERT_EQUAL_UINT8(0x01, buf[16]);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[17]);
    TEST_ASSERT_EQUAL_UINT8(0x01, buf[20]);  // outProtoMask = UBX
    // Checksum self-consistency.
    uint8_t a, b;
    ubxChecksum(buf + 2, 4 + 20, a, b);
    TEST_ASSERT_EQUAL_UINT8(a, buf[26]);
    TEST_ASSERT_EQUAL_UINT8(b, buf[27]);
}

void test_buffer_too_small_returns_zero() {
    uint8_t buf[4];
    TEST_ASSERT_EQUAL_UINT32(0, buildCfgRate(100, buf, sizeof(buf)));
}

// ---- NAV-PVT decode ------------------------------------------------------

void test_decode_nav_pvt() {
    auto p = navPvtPayload();
    UbxFix fix{};
    TEST_ASSERT_TRUE(decodeNavPvt(p.data(), static_cast<uint16_t>(p.size()), fix));
    TEST_ASSERT_TRUE(fix.valid);
    TEST_ASSERT_EQUAL_UINT8(9, fix.num_sat);
    TEST_ASSERT_EQUAL_INT32(451715460, fix.lat);
    TEST_ASSERT_EQUAL_INT32(57223870, fix.lon);
    TEST_ASSERT_EQUAL_INT16(120, fix.alt_m);
    TEST_ASSERT_EQUAL_UINT16(500, fix.speed_cms);
    TEST_ASSERT_EQUAL_UINT16(900, fix.course_ddeg);
}

void test_decode_nav_pvt_no_fix() {
    auto p = navPvtPayload();
    p[20] = 0;     // fixType none
    p[21] = 0x00;  // gnssFixOK clear
    UbxFix fix{};
    TEST_ASSERT_TRUE(decodeNavPvt(p.data(), static_cast<uint16_t>(p.size()), fix));
    TEST_ASSERT_FALSE(fix.valid);
}

void test_decode_nav_pvt_too_short() {
    uint8_t p[50] = {0};
    UbxFix fix{};
    TEST_ASSERT_FALSE(decodeNavPvt(p, sizeof(p), fix));
}

// ---- Parser --------------------------------------------------------------

void test_parser_reads_nav_pvt_frame() {
    auto frame = makeFrame(kUbxClassNav, kUbxIdNavPvt, navPvtPayload());
    UbxParser parser;
    int done = 0;
    for (uint8_t b : frame) {
        if (parser.feed(b)) done++;
    }
    TEST_ASSERT_EQUAL_INT(1, done);
    TEST_ASSERT_EQUAL_UINT8(kUbxClassNav, parser.msgClass());
    TEST_ASSERT_EQUAL_UINT8(kUbxIdNavPvt, parser.msgId());
    TEST_ASSERT_EQUAL_UINT16(92, parser.length());

    UbxFix fix{};
    TEST_ASSERT_TRUE(decodeNavPvt(parser.payload(), parser.length(), fix));
    TEST_ASSERT_EQUAL_INT32(451715460, fix.lat);
}

void test_parser_rejects_bad_checksum() {
    auto frame = makeFrame(kUbxClassNav, kUbxIdNavPvt, navPvtPayload());
    frame.back() ^= 0xFF;
    UbxParser parser;
    int done = 0;
    for (uint8_t b : frame) {
        if (parser.feed(b)) done++;
    }
    TEST_ASSERT_EQUAL_INT(0, done);
}

void test_parser_resyncs_after_garbage() {
    UbxParser parser;
    std::vector<uint8_t> stream = {0x00, 0xB5, 0x11, 0xFF};  // noise incl false sync
    auto frame = makeFrame(kUbxClassNav, kUbxIdNavPvt, navPvtPayload());
    stream.insert(stream.end(), frame.begin(), frame.end());
    int done = 0;
    for (uint8_t b : stream) {
        if (parser.feed(b)) done++;
    }
    TEST_ASSERT_EQUAL_INT(1, done);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_cfg_rate_matches_known_10hz_command);
    RUN_TEST(test_cfg_msg_enables_nav_pvt);
    RUN_TEST(test_cfg_prt_sets_baud_and_ubx_out);
    RUN_TEST(test_buffer_too_small_returns_zero);
    RUN_TEST(test_decode_nav_pvt);
    RUN_TEST(test_decode_nav_pvt_no_fix);
    RUN_TEST(test_decode_nav_pvt_too_short);
    RUN_TEST(test_parser_reads_nav_pvt_frame);
    RUN_TEST(test_parser_rejects_bad_checksum);
    RUN_TEST(test_parser_resyncs_after_garbage);
    return UNITY_END();
}
