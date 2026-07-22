#include <unity.h>

#include <cstring>

#include "protocol.h"

using namespace ff;

void setUp() {}
void tearDown() {}

// ---- Position round-trip -------------------------------------------------

void test_position_roundtrip() {
    PositionPacket in{};
    in.uid = 0xDEADBEEF;
    in.lat = 451715460;      // 45.171546 deg
    in.lon = 57223870;       // 5.722387 deg
    in.alt_m = -120;
    in.speed_cms = 4200;
    in.course_ddeg = 3599;
    in.flags = POSITION_FLAG_ARMED | POSITION_FLAG_HAS_FIX;

    uint8_t buf[64];
    size_t n = encodePosition(in, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(kPositionPacketSize, n);

    PositionPacket out{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decodePosition(buf, n, out)));
    TEST_ASSERT_EQUAL_UINT32(in.uid, out.uid);
    TEST_ASSERT_EQUAL_INT32(in.lat, out.lat);
    TEST_ASSERT_EQUAL_INT32(in.lon, out.lon);
    TEST_ASSERT_EQUAL_INT16(in.alt_m, out.alt_m);
    TEST_ASSERT_EQUAL_UINT16(in.speed_cms, out.speed_cms);
    TEST_ASSERT_EQUAL_UINT16(in.course_ddeg, out.course_ddeg);
    TEST_ASSERT_EQUAL_UINT8(in.flags, out.flags);
}

void test_position_wire_is_little_endian() {
    PositionPacket in{};
    in.uid = 0x04030201;
    uint8_t buf[64];
    size_t n = encodePosition(in, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(kPositionPacketSize, n);
    // Header: version, type, then uid little-endian.
    TEST_ASSERT_EQUAL_UINT8(kProtocolVersion, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PacketType::Position), buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0x01, buf[2]);
    TEST_ASSERT_EQUAL_UINT8(0x02, buf[3]);
    TEST_ASSERT_EQUAL_UINT8(0x03, buf[4]);
    TEST_ASSERT_EQUAL_UINT8(0x04, buf[5]);
}

void test_position_encode_buffer_too_small() {
    PositionPacket in{};
    uint8_t buf[kPositionPacketSize - 1];
    TEST_ASSERT_EQUAL_UINT32(0, encodePosition(in, buf, sizeof(buf)));
}

void test_position_decode_too_short() {
    PositionPacket in{};
    uint8_t buf[64];
    size_t n = encodePosition(in, buf, sizeof(buf));
    PositionPacket out{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::TooShort),
                      static_cast<int>(decodePosition(buf, n - 1, out)));
}

// ---- Announce round-trip -------------------------------------------------

void test_announce_roundtrip() {
    AnnouncePacket in{};
    in.uid = 0x11223344;
    std::strcpy(in.name, "FALCON1");
    in.capabilities = CAP_HAS_GPS | CAP_HAS_MSP_FC;

    uint8_t buf[64];
    size_t n = encodeAnnounce(in, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(kHeaderSize + 1 + 7 + 4, n);

    AnnouncePacket out{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decodeAnnounce(buf, n, out)));
    TEST_ASSERT_EQUAL_UINT32(in.uid, out.uid);
    TEST_ASSERT_EQUAL_STRING(in.name, out.name);
    TEST_ASSERT_EQUAL_UINT32(in.capabilities, out.capabilities);
}

void test_announce_empty_name() {
    AnnouncePacket in{};
    in.uid = 1;
    in.name[0] = '\0';
    in.capabilities = 0;
    uint8_t buf[64];
    size_t n = encodeAnnounce(in, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(kHeaderSize + 1 + 0 + 4, n);

    AnnouncePacket out{};
    out.name[0] = 'X';
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decodeAnnounce(buf, n, out)));
    TEST_ASSERT_EQUAL_STRING("", out.name);
}

void test_announce_name_is_capped_to_max() {
    // Fill the whole name buffer with no null terminator; the encoder must cap
    // the emitted length at kMaxNameLen rather than run off the end.
    AnnouncePacket in{};
    in.uid = 1;
    std::memset(in.name, 'A', sizeof(in.name));  // no terminator anywhere
    uint8_t buf[64];
    size_t n = encodeAnnounce(in, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(kHeaderSize + 1 + kMaxNameLen + 4, n);

    AnnouncePacket out{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decodeAnnounce(buf, n, out)));
    TEST_ASSERT_EQUAL_UINT32(kMaxNameLen, std::strlen(out.name));
    TEST_ASSERT_EQUAL_STRING("AAAAAAAAAAAAAAA", out.name);  // 15 chars
}

// ---- Header triage -------------------------------------------------------

void test_peek_bad_version() {
    uint8_t buf[kPositionPacketSize] = {0};
    buf[0] = 99;  // wrong version
    buf[1] = static_cast<uint8_t>(PacketType::Position);
    Header hdr;
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::BadVersion),
                      static_cast<int>(peekHeader(buf, sizeof(buf), hdr)));
}

void test_peek_bad_type() {
    uint8_t buf[kPositionPacketSize] = {0};
    buf[0] = kProtocolVersion;
    buf[1] = 0;  // type 0 is invalid (all-zeros packet must not decode)
    Header hdr;
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::BadType),
                      static_cast<int>(peekHeader(buf, sizeof(buf), hdr)));
}

void test_decode_wrong_type_rejected() {
    // Encode an announce, try to decode it as a position.
    AnnouncePacket in{};
    in.uid = 7;
    std::strcpy(in.name, "X");
    uint8_t buf[64];
    size_t n = encodeAnnounce(in, buf, sizeof(buf));
    PositionPacket out{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::BadType),
                      static_cast<int>(decodePosition(buf, n, out)));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_position_roundtrip);
    RUN_TEST(test_position_wire_is_little_endian);
    RUN_TEST(test_position_encode_buffer_too_small);
    RUN_TEST(test_position_decode_too_short);
    RUN_TEST(test_announce_roundtrip);
    RUN_TEST(test_announce_empty_name);
    RUN_TEST(test_announce_name_is_capped_to_max);
    RUN_TEST(test_peek_bad_version);
    RUN_TEST(test_peek_bad_type);
    RUN_TEST(test_decode_wrong_type_rejected);
    return UNITY_END();
}
