#include <unity.h>

#include <cstring>
#include <vector>

#include "msp_crc.h"
#include "msp_fc.h"
#include "msp_parser.h"
#include "wire.h"

using namespace ff;

void setUp() {}
void tearDown() {}

// ---- Frame builders ---------------------------------------------------------

void test_v1_request_frame_layout_and_crc() {
    uint8_t buf[16];
    const size_t n = buildMspV1(kMspRawGps, nullptr, 0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(6, n);
    const uint8_t expected[] = {'$', 'M', '<', 0, 106, 106};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, buf, 6);
}

void test_v1_command_frame_crc_covers_payload() {
    uint8_t payload[] = {0x01, 0x02, 0x04};
    uint8_t buf[16];
    const size_t n = buildMspV1(kMspSetHead, payload, sizeof(payload), buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(9, n);
    TEST_ASSERT_EQUAL_UINT8(3, buf[3]);
    TEST_ASSERT_EQUAL_UINT8(kMspSetHead, buf[4]);
    TEST_ASSERT_EQUAL_UINT8(3 ^ kMspSetHead ^ 0x01 ^ 0x02 ^ 0x04, buf[8]);
}

void test_v1_frame_rejects_small_buffer() {
    uint8_t buf[5];
    TEST_ASSERT_EQUAL_size_t(0, buildMspV1(kMspRawGps, nullptr, 0, buf, sizeof(buf)));
}

void test_v2_frame_layout_and_crc() {
    uint8_t payload[] = {0xAA, 0x55};
    uint8_t buf[16];
    const size_t n = buildMspV2(kMsp2InavSetGvar, payload, sizeof(payload), buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(11, n);
    TEST_ASSERT_EQUAL_UINT8('$', buf[0]);
    TEST_ASSERT_EQUAL_UINT8('X', buf[1]);
    TEST_ASSERT_EQUAL_UINT8('<', buf[2]);
    TEST_ASSERT_EQUAL_UINT8(0, buf[3]);
    TEST_ASSERT_EQUAL_UINT8(0x14, buf[4]);  // id lo
    TEST_ASSERT_EQUAL_UINT8(0x22, buf[5]);  // id hi
    TEST_ASSERT_EQUAL_UINT8(2, buf[6]);
    TEST_ASSERT_EQUAL_UINT8(0, buf[7]);
    uint8_t crc = 0;
    for (size_t i = 3; i < 10; i++) crc = mspCrc8DvbS2(crc, buf[i]);
    TEST_ASSERT_EQUAL_UINT8(crc, buf[10]);
}

// A v2 frame we build must parse back through MspParser once the direction
// byte is flipped to a reply -- the two halves of the codec agree.
void test_v2_frame_round_trips_through_parser() {
    uint8_t payload[] = {1, 2, 3, 4, 5, 6, 7};
    uint8_t buf[32];
    const size_t n = buildMspV2(kMsp2InavMixer, payload, sizeof(payload), buf, sizeof(buf));
    buf[2] = '>';
    MspParser p;
    bool done = false;
    for (size_t i = 0; i < n; i++) done = p.feed(buf[i]);
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQUAL_UINT8(2, p.version());
    TEST_ASSERT_EQUAL_UINT16(kMsp2InavMixer, p.id());
    TEST_ASSERT_EQUAL_UINT16(7, p.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, p.payload(), 7);
}

// ---- Decoders ---------------------------------------------------------------

void test_decode_raw_gps() {
    // fix=3 sats=12 lat=37.0 lon=-122.0 alt=100 speed=1234 course=1800 hdop=90,
    // laid out exactly as INAV's MSP_RAW_GPS reply (all little-endian).
    uint8_t p[18];
    uint8_t* w = p;
    wire::put_u8(w, 3);
    wire::put_u8(w, 12);
    wire::put_i32(w, 370000000);
    wire::put_i32(w, -1220000000);
    wire::put_i16(w, 100);
    wire::put_i16(w, 1234);
    wire::put_i16(w, 1800);
    wire::put_u16(w, 90);
    FcRawGps g;
    TEST_ASSERT_TRUE(decodeRawGps(p, sizeof(p), g));
    TEST_ASSERT_EQUAL_UINT8(3, g.fix_type);
    TEST_ASSERT_EQUAL_UINT8(12, g.num_sat);
    TEST_ASSERT_EQUAL_INT32(370000000, g.lat);
    TEST_ASSERT_EQUAL_INT32(-1220000000, g.lon);
    TEST_ASSERT_EQUAL_INT16(100, g.alt_m);
    TEST_ASSERT_EQUAL_INT16(1234, g.speed_cms);
    TEST_ASSERT_EQUAL_INT16(1800, g.course_ddeg);
    TEST_ASSERT_EQUAL_UINT16(90, g.hdop);
    TEST_ASSERT_FALSE(decodeRawGps(p, 17, g));
}

void test_decode_altitude_cm() {
    const uint8_t p[] = {0x18, 0xFC, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0};  // -1000
    int32_t cm = 0;
    TEST_ASSERT_TRUE(decodeAltitudeCm(p, sizeof(p), cm));
    TEST_ASSERT_EQUAL_INT32(-1000, cm);
    TEST_ASSERT_FALSE(decodeAltitudeCm(p, 3, cm));
}

void test_decode_rc_channels() {
    const uint8_t p[] = {0xE8, 0x03, 0xDC, 0x05, 0xD0, 0x07};  // 1000 1500 2000
    uint16_t ch[16] = {0};
    TEST_ASSERT_EQUAL_size_t(3, decodeRc(p, sizeof(p), ch, 16));
    TEST_ASSERT_EQUAL_UINT16(1000, ch[0]);
    TEST_ASSERT_EQUAL_UINT16(1500, ch[1]);
    TEST_ASSERT_EQUAL_UINT16(2000, ch[2]);
    TEST_ASSERT_EQUAL_size_t(2, decodeRc(p, sizeof(p), ch, 2));  // capped
}

void test_decode_fc_version_and_variant() {
    const uint8_t v[] = {9, 1, 0};
    FcVersion ver;
    TEST_ASSERT_TRUE(decodeFcVersion(v, 3, ver));
    TEST_ASSERT_EQUAL_UINT8(9, ver.major);
    TEST_ASSERT_EQUAL_UINT8(1, ver.minor);

    const uint8_t id[] = {'I', 'N', 'A', 'V'};
    char variant[5];
    TEST_ASSERT_TRUE(decodeFcVariant(id, 4, variant));
    TEST_ASSERT_EQUAL_STRING("INAV", variant);
    TEST_ASSERT_TRUE(fcVariantIsInav(variant));
    const uint8_t btfl[] = {'B', 'T', 'F', 'L'};
    decodeFcVariant(btfl, 4, variant);
    TEST_ASSERT_FALSE(fcVariantIsInav(variant));
}

void test_decode_mixer_platform() {
    const uint8_t p[] = {0, 0, 0, 1, 0, 0, 0, 8, 8};  // platformType = AIRPLANE
    FcPlatform plat = FcPlatform::Unknown;
    TEST_ASSERT_TRUE(decodeMixerPlatform(p, sizeof(p), plat));
    TEST_ASSERT_EQUAL(static_cast<int>(FcPlatform::Airplane), static_cast<int>(plat));
    TEST_ASSERT_FALSE(decodeMixerPlatform(p, 3, plat));
}

// ---- Mode mapping -----------------------------------------------------------

void test_active_modes_map_box_index_through_boxids() {
    // FC's enabled boxes, in its order: ARM(0), ANGLE(1), NAVPOSHOLD(11), GCSNAV(31)
    const uint8_t boxIds[] = {0, 1, 11, 31};
    // Bits set for index 0 (ARM) and index 3 (GCSNAV): 0b1001
    const uint8_t flags[] = {0x09, 0, 0, 0};
    const uint32_t modes = decodeActiveModes(flags, 4, boxIds, 4);
    TEST_ASSERT_TRUE(modeActive(modes, FC_MODE_ARM));
    TEST_ASSERT_FALSE(modeActive(modes, FC_MODE_ANGLE));
    TEST_ASSERT_FALSE(modeActive(modes, FC_MODE_NAVPOSHOLD));
    TEST_ASSERT_TRUE(modeActive(modes, FC_MODE_GCSNAV));
    TEST_ASSERT_FALSE(modeActive(modes, FC_MODE_MAG));
}

void test_active_modes_reach_past_bit_31_with_full_width_mask() {
    // 40 enabled boxes; GCSNAV (permanent id 31) sits at index 35 -- invisible
    // to MSP_STATUS's 32-bit field, visible in MSP2_INAV_STATUS's full mask.
    uint8_t boxIds[40];
    for (uint8_t i = 0; i < 40; i++) boxIds[i] = 100 + i;  // ids we don't map
    boxIds[35] = 31;                                       // GCSNAV
    boxIds[2] = 5;                                         // MAG at index 2
    uint8_t flags[8] = {0};
    flags[35 / 8] |= static_cast<uint8_t>(1u << (35 % 8));
    flags[2 / 8] |= static_cast<uint8_t>(1u << (2 % 8));

    const uint32_t full = decodeActiveModes(flags, 8, boxIds, 40);
    TEST_ASSERT_TRUE(modeActive(full, FC_MODE_GCSNAV));
    TEST_ASSERT_TRUE(modeActive(full, FC_MODE_MAG));

    // The same FC state through MSP_STATUS's 4-byte mask loses GCSNAV.
    const uint32_t narrow = decodeActiveModes(flags, 4, boxIds, 40);
    TEST_ASSERT_FALSE(modeActive(narrow, FC_MODE_GCSNAV));
    TEST_ASSERT_TRUE(modeActive(narrow, FC_MODE_MAG));
}

void test_status_mode_flag_locators() {
    // MSP_STATUS: 2+2+2 then flags(4) then profile
    const uint8_t v1[] = {0, 0, 0, 0, 0, 0, 0xAA, 0xBB, 0xCC, 0xDD, 1};
    const uint8_t* flags;
    size_t n;
    TEST_ASSERT_TRUE(statusModeFlags(v1, sizeof(v1), flags, n));
    TEST_ASSERT_EQUAL_size_t(4, n);
    TEST_ASSERT_EQUAL_UINT8(0xAA, flags[0]);
    TEST_ASSERT_FALSE(statusModeFlags(v1, 9, flags, n));

    // MSP2_INAV_STATUS: 13 header bytes, 8 mask bytes, 1 mixer-profile byte
    uint8_t v2[13 + 8 + 1] = {0};
    v2[9] = 0x04;  // armingFlags bit 2 = ARMED
    v2[13] = 0x01;
    v2[20] = 0x80;
    TEST_ASSERT_TRUE(inavStatusModeFlags(v2, sizeof(v2), flags, n));
    TEST_ASSERT_EQUAL_size_t(9, n);  // everything after the header
    TEST_ASSERT_EQUAL_UINT8(0x01, flags[0]);
    bool armed = false;
    TEST_ASSERT_TRUE(inavStatusArmed(v2, sizeof(v2), armed));
    TEST_ASSERT_TRUE(armed);
    v2[9] = 0;
    inavStatusArmed(v2, sizeof(v2), armed);
    TEST_ASSERT_FALSE(armed);
}

// ---- Command payloads -------------------------------------------------------

void test_encode_set_wp_255() {
    uint8_t buf[kMspSetWpPayloadSize];
    const size_t n = encodeSetWp(255, 1, 370000000, -1220000000, 2000, 45, 0, 0, 0, buf,
                                 sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(21, n);
    TEST_ASSERT_EQUAL_UINT8(255, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(1, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0x80, buf[2]);  // lat lo byte of 370000000
    TEST_ASSERT_EQUAL_UINT8(0xD0, buf[10]);  // alt 2000 = 0x07D0
    TEST_ASSERT_EQUAL_UINT8(0x07, buf[11]);
    TEST_ASSERT_EQUAL_UINT8(45, buf[14]);   // p1 lo
    TEST_ASSERT_EQUAL_UINT8(0, buf[20]);    // flag
    TEST_ASSERT_EQUAL_size_t(0, encodeSetWp(255, 1, 0, 0, 0, 0, 0, 0, 0, buf, 20));
}

void test_encode_set_head_and_gvar() {
    uint8_t h[2];
    TEST_ASSERT_EQUAL_size_t(2, encodeSetHead(359, h, sizeof(h)));
    TEST_ASSERT_EQUAL_UINT8(0x67, h[0]);
    TEST_ASSERT_EQUAL_UINT8(0x01, h[1]);

    uint8_t g[5];
    TEST_ASSERT_EQUAL_size_t(5, encodeSetGvar(3, -2, g, sizeof(g)));
    TEST_ASSERT_EQUAL_UINT8(3, g[0]);
    TEST_ASSERT_EQUAL_UINT8(0xFE, g[1]);
    TEST_ASSERT_EQUAL_UINT8(0xFF, g[4]);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_v1_request_frame_layout_and_crc);
    RUN_TEST(test_v1_command_frame_crc_covers_payload);
    RUN_TEST(test_v1_frame_rejects_small_buffer);
    RUN_TEST(test_v2_frame_layout_and_crc);
    RUN_TEST(test_v2_frame_round_trips_through_parser);
    RUN_TEST(test_decode_raw_gps);
    RUN_TEST(test_decode_altitude_cm);
    RUN_TEST(test_decode_rc_channels);
    RUN_TEST(test_decode_fc_version_and_variant);
    RUN_TEST(test_decode_mixer_platform);
    RUN_TEST(test_active_modes_map_box_index_through_boxids);
    RUN_TEST(test_active_modes_reach_past_bit_31_with_full_width_mask);
    RUN_TEST(test_status_mode_flag_locators);
    RUN_TEST(test_encode_set_wp_255);
    RUN_TEST(test_encode_set_head_and_gvar);
    return UNITY_END();
}
