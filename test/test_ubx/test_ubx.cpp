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
    TEST_ASSERT_EQUAL_UINT8(3, fix.fix_type);
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
    TEST_ASSERT_EQUAL_UINT8(0, fix.fix_type);
}

void test_decode_nav_pvt_keeps_a_two_d_fix_distinct_from_a_three_d_one() {
    auto p = navPvtPayload();
    p[20] = 2;     // fixType 2D
    UbxFix fix{};
    TEST_ASSERT_TRUE(decodeNavPvt(p.data(), static_cast<uint16_t>(p.size()), fix));
    TEST_ASSERT_TRUE(fix.valid);
    TEST_ASSERT_EQUAL_UINT8(2, fix.fix_type);
}

// A receiver can report a 3D solution while telling us not to trust it. That is
// worse than no fix at all if it is passed on as a 3D one, because everything
// downstream treats it as the best quality there is.
void test_a_three_d_solution_the_receiver_distrusts_is_reported_as_no_fix() {
    auto p = navPvtPayload();
    p[20] = 3;     // fixType 3D
    p[21] = 0x00;  // gnssFixOK clear
    UbxFix fix{};
    TEST_ASSERT_TRUE(decodeNavPvt(p.data(), static_cast<uint16_t>(p.size()), fix));
    TEST_ASSERT_FALSE(fix.valid);
    TEST_ASSERT_EQUAL_UINT8(0, fix.fix_type);
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


// ---- The legacy navigation set ------------------------------------------------
//
// NAV-PVT arrived in u-blox protocol 14. A NEO-6M, which is what a lot of
// T-Beams are actually fitted with, NAKs the request for it and then emits
// nothing at all, which from the outside is indistinguishable from a module
// that is not wired up. These four older messages carry the same information
// between them.

static void putU16(uint8_t* p, size_t off, uint16_t v) {
    p[off] = v & 0xFF;
    p[off + 1] = (v >> 8) & 0xFF;
}

void test_decode_nav_posllh() {
    std::vector<uint8_t> p(28, 0);
    putI32(p.data(), 4, 57223870);    // lon
    putI32(p.data(), 8, 451715460);   // lat
    putI32(p.data(), 12, 168000);     // height above ellipsoid, the wrong one
    putI32(p.data(), 16, 120000);     // hMSL 120 m, in mm
    UbxPosLlh out{};
    TEST_ASSERT_TRUE(decodeNavPosllh(p.data(), static_cast<uint16_t>(p.size()), out));
    TEST_ASSERT_EQUAL_INT32(451715460, out.lat);
    TEST_ASSERT_EQUAL_INT32(57223870, out.lon);
    // The ellipsoid height differs by tens of metres and is the classic thing
    // to read by mistake, so this asserts the altitude is the MSL one.
    TEST_ASSERT_EQUAL_INT16(120, out.alt_m);
}

void test_decode_nav_posllh_too_short() {
    uint8_t p[20] = {0};
    UbxPosLlh out{};
    TEST_ASSERT_FALSE(decodeNavPosllh(p, sizeof(p), out));
}

void test_decode_nav_sol() {
    std::vector<uint8_t> p(52, 0);
    p[10] = 3;     // gpsFix = 3D
    p[11] = 0x01;  // flags: GPSfixOK
    p[47] = 11;    // numSV
    UbxSol out{};
    TEST_ASSERT_TRUE(decodeNavSol(p.data(), static_cast<uint16_t>(p.size()), out));
    TEST_ASSERT_TRUE(out.valid);
    TEST_ASSERT_EQUAL_UINT8(3, out.fix_type);
    TEST_ASSERT_EQUAL_UINT8(11, out.num_sat);
}

// Same rule as NAV-PVT: a solution the receiver itself does not trust is
// reported as no fix, not as the quality it claims.
void test_nav_sol_without_the_ok_flag_is_no_fix() {
    std::vector<uint8_t> p(52, 0);
    p[10] = 3;     // claims 3D
    p[11] = 0x00;  // GPSfixOK clear
    p[47] = 4;
    UbxSol out{};
    TEST_ASSERT_TRUE(decodeNavSol(p.data(), static_cast<uint16_t>(p.size()), out));
    TEST_ASSERT_FALSE(out.valid);
    TEST_ASSERT_EQUAL_UINT8(0, out.fix_type);
    // The satellite count is still worth having: it is what says whether the
    // receiver is close to a fix or nowhere near one.
    TEST_ASSERT_EQUAL_UINT8(4, out.num_sat);
}

void test_nav_sol_keeps_two_d_distinct_from_three_d() {
    std::vector<uint8_t> p(52, 0);
    p[10] = 2;
    p[11] = 0x01;
    UbxSol out{};
    TEST_ASSERT_TRUE(decodeNavSol(p.data(), static_cast<uint16_t>(p.size()), out));
    TEST_ASSERT_TRUE(out.valid);
    TEST_ASSERT_EQUAL_UINT8(2, out.fix_type);
}

void test_decode_nav_velned() {
    std::vector<uint8_t> p(36, 0);
    putI32(p.data(), 16, 700);     // 3D speed, higher because it is climbing
    putI32(p.data(), 20, 500);     // gSpeed 5.00 m/s
    putI32(p.data(), 24, 9000000); // heading 90.00000 deg
    UbxVelNed out{};
    TEST_ASSERT_TRUE(decodeNavVelned(p.data(), static_cast<uint16_t>(p.size()), out));
    // Ground speed, not 3D speed. They differ whenever the aircraft climbs and
    // the wrong one makes every distance calculation downstream slightly long.
    TEST_ASSERT_EQUAL_UINT16(500, out.speed_cms);
    TEST_ASSERT_EQUAL_UINT16(900, out.course_ddeg);
}

void test_nav_velned_course_wraps_and_never_goes_negative() {
    std::vector<uint8_t> p(36, 0);
    putI32(p.data(), 24, -9000000);  // -90 deg
    UbxVelNed out{};
    TEST_ASSERT_TRUE(decodeNavVelned(p.data(), static_cast<uint16_t>(p.size()), out));
    TEST_ASSERT_EQUAL_UINT16(2700, out.course_ddeg);
}

void test_decode_nav_dop() {
    std::vector<uint8_t> p(18, 0);
    putU16(p.data(), 6, 250);   // pDOP, the neighbouring field
    putU16(p.data(), 12, 131);  // hDOP 1.31
    uint16_t hdop = 0;
    TEST_ASSERT_TRUE(decodeNavDop(p.data(), static_cast<uint16_t>(p.size()), hdop));
    TEST_ASSERT_EQUAL_UINT16(131, hdop);
}

void test_decode_ack_reports_what_was_acknowledged() {
    const uint8_t p[2] = {kUbxClassCfg, kUbxIdCfgMsg};
    uint8_t cls = 0, id = 0;
    TEST_ASSERT_TRUE(decodeAck(p, sizeof(p), cls, id));
    TEST_ASSERT_EQUAL_UINT8(kUbxClassCfg, cls);
    TEST_ASSERT_EQUAL_UINT8(kUbxIdCfgMsg, id);
}

void test_decode_ack_too_short() {
    const uint8_t p[1] = {kUbxClassCfg};
    uint8_t cls = 0, id = 0;
    TEST_ASSERT_FALSE(decodeAck(p, sizeof(p), cls, id));
}

// Four CFG-MSG frames, each enabling one of the legacy messages at every
// navigation solution.
void test_legacy_nav_config_enables_all_four_messages() {
    uint8_t buf[64];
    const size_t n = buildLegacyNavConfig(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(4 * 11, n);  // 8 bytes of framing + 3 of payload

    const uint8_t expect_ids[4] = {kUbxIdNavPosllh, kUbxIdNavSol, kUbxIdNavVelned,
                                   kUbxIdNavDop};
    for (size_t i = 0; i < 4; i++) {
        const uint8_t* f = buf + i * 11;
        TEST_ASSERT_EQUAL_UINT8(0xB5, f[0]);
        TEST_ASSERT_EQUAL_UINT8(0x62, f[1]);
        TEST_ASSERT_EQUAL_UINT8(kUbxClassCfg, f[2]);
        TEST_ASSERT_EQUAL_UINT8(kUbxIdCfgMsg, f[3]);
        TEST_ASSERT_EQUAL_UINT8(kUbxClassNav, f[6]);  // payload: class being set
        TEST_ASSERT_EQUAL_UINT8(expect_ids[i], f[7]);
        TEST_ASSERT_EQUAL_UINT8(1, f[8]);  // every solution
        uint8_t a, b;
        ubxChecksum(f + 2, 4 + 3, a, b);
        TEST_ASSERT_EQUAL_UINT8(a, f[9]);
        TEST_ASSERT_EQUAL_UINT8(b, f[10]);
    }
}

void test_legacy_nav_config_refuses_a_short_buffer() {
    uint8_t buf[20];
    TEST_ASSERT_EQUAL_size_t(0, buildLegacyNavConfig(buf, sizeof(buf)));
}

// The parser has to hand these up the same way it hands up NAV-PVT, or the
// driver never sees them.
void test_parser_reads_a_legacy_nav_frame() {
    std::vector<uint8_t> payload(52, 0);
    payload[10] = 3;
    payload[11] = 0x01;
    payload[47] = 7;
    const std::vector<uint8_t> frame = makeFrame(kUbxClassNav, kUbxIdNavSol, payload);

    UbxParser parser;
    bool complete = false;
    for (size_t i = 0; i < frame.size(); i++) {
        complete = parser.feed(frame[i]);
    }
    TEST_ASSERT_TRUE(complete);
    TEST_ASSERT_EQUAL_UINT8(kUbxClassNav, parser.msgClass());
    TEST_ASSERT_EQUAL_UINT8(kUbxIdNavSol, parser.msgId());

    UbxSol out{};
    TEST_ASSERT_TRUE(decodeNavSol(parser.payload(), parser.length(), out));
    TEST_ASSERT_EQUAL_UINT8(7, out.num_sat);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_cfg_rate_matches_known_10hz_command);
    RUN_TEST(test_cfg_msg_enables_nav_pvt);
    RUN_TEST(test_cfg_prt_sets_baud_and_ubx_out);
    RUN_TEST(test_buffer_too_small_returns_zero);
    RUN_TEST(test_decode_nav_pvt);
    RUN_TEST(test_decode_nav_pvt_no_fix);
    RUN_TEST(test_decode_nav_pvt_keeps_a_two_d_fix_distinct_from_a_three_d_one);
    RUN_TEST(test_a_three_d_solution_the_receiver_distrusts_is_reported_as_no_fix);
    RUN_TEST(test_decode_nav_pvt_too_short);
    RUN_TEST(test_parser_reads_nav_pvt_frame);
    RUN_TEST(test_parser_rejects_bad_checksum);
    RUN_TEST(test_parser_resyncs_after_garbage);

    RUN_TEST(test_decode_nav_posllh);
    RUN_TEST(test_decode_nav_posllh_too_short);
    RUN_TEST(test_decode_nav_sol);
    RUN_TEST(test_nav_sol_without_the_ok_flag_is_no_fix);
    RUN_TEST(test_nav_sol_keeps_two_d_distinct_from_three_d);
    RUN_TEST(test_decode_nav_velned);
    RUN_TEST(test_nav_velned_course_wraps_and_never_goes_negative);
    RUN_TEST(test_decode_nav_dop);
    RUN_TEST(test_decode_ack_reports_what_was_acknowledged);
    RUN_TEST(test_decode_ack_too_short);
    RUN_TEST(test_legacy_nav_config_enables_all_four_messages);
    RUN_TEST(test_legacy_nav_config_refuses_a_short_buffer);
    RUN_TEST(test_parser_reads_a_legacy_nav_frame);
    return UNITY_END();
}
