// AES-128, SHA-256, CCM and the v2 frame cipher.
//
// The primitives are checked against published vectors rather than against
// themselves: a round-trip test passes just as happily with a wrong-but-
// consistent implementation, which would then fail to talk to anything else and
// would not provide the security it claims.

#include <unity.h>

#include <cstring>
#include <vector>

#include "aes.h"
#include "ccm.h"
#include "crypto.h"
#include "protocol.h"
#include "sha256.h"

using namespace ff;

void setUp() {}
void tearDown() {}

// Collects hex digits, ignoring separators. Fails loudly on an odd digit count
// rather than walking off the end of the string, which is what the first
// version of this helper did: a mistyped vector then produced a short buffer and
// a confusing length mismatch instead of naming the real problem.
static std::vector<uint8_t> hexToBytes(const char* hex) {
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<int> nibbles;
    for (const char* p = hex; *p != '\0'; p++) {
        const int n = nib(*p);
        if (n >= 0) {
            nibbles.push_back(n);
        }
    }
    TEST_ASSERT_EQUAL_MESSAGE(0, nibbles.size() % 2, "hex literal has an odd digit count");
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < nibbles.size(); i += 2) {
        out.push_back(static_cast<uint8_t>((nibbles[i] << 4) | nibbles[i + 1]));
    }
    return out;
}

// ---- AES-128, FIPS-197 Appendix C.1 -----------------------------------------

void test_aes128_fips197_vector() {
    auto key = hexToBytes("000102030405060708090a0b0c0d0e0f");
    auto pt = hexToBytes("00112233445566778899aabbccddeeff");
    auto expect = hexToBytes("69c4e0d86a7b0430d8cdb78070b4c55a");

    Aes128 aes;
    aes.setKey(key.data());
    uint8_t block[16];
    std::memcpy(block, pt.data(), 16);
    aes.encryptBlock(block);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expect.data(), block, 16);
}

// FIPS-197 Appendix B, the "worked example" with a different key.
void test_aes128_fips197_appendix_b() {
    auto key = hexToBytes("2b7e151628aed2a6abf7158809cf4f3c");
    auto pt = hexToBytes("3243f6a8885a308d313198a2e0370734");
    auto expect = hexToBytes("3925841d02dc09fbdc118597196a0b32");

    Aes128 aes;
    aes.setKey(key.data());
    uint8_t block[16];
    std::memcpy(block, pt.data(), 16);
    aes.encryptBlock(block);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expect.data(), block, 16);
}

void test_aes128_rekey_is_clean() {
    auto k1 = hexToBytes("000102030405060708090a0b0c0d0e0f");
    auto k2 = hexToBytes("2b7e151628aed2a6abf7158809cf4f3c");
    auto pt = hexToBytes("3243f6a8885a308d313198a2e0370734");
    auto expect = hexToBytes("3925841d02dc09fbdc118597196a0b32");

    Aes128 aes;
    aes.setKey(k1.data());
    uint8_t scratch[16];
    std::memcpy(scratch, pt.data(), 16);
    aes.encryptBlock(scratch);
    // Re-keying must fully replace the schedule, not mix with the old one.
    aes.setKey(k2.data());
    std::memcpy(scratch, pt.data(), 16);
    aes.encryptBlock(scratch);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expect.data(), scratch, 16);
}

// ---- SHA-256, NIST examples --------------------------------------------------

void test_sha256_abc() {
    auto expect = hexToBytes("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    uint8_t digest[kSha256DigestSize];
    sha256(reinterpret_cast<const uint8_t*>("abc"), 3, digest);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expect.data(), digest, kSha256DigestSize);
}

void test_sha256_empty() {
    auto expect = hexToBytes("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    uint8_t digest[kSha256DigestSize];
    sha256(reinterpret_cast<const uint8_t*>(""), 0, digest);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expect.data(), digest, kSha256DigestSize);
}

// Two-block message: exercises the length-padding path across a block boundary.
void test_sha256_two_block() {
    const char* msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    auto expect = hexToBytes("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    uint8_t digest[kSha256DigestSize];
    sha256(reinterpret_cast<const uint8_t*>(msg), std::strlen(msg), digest);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expect.data(), digest, kSha256DigestSize);
}

// Fed one byte at a time, the incremental API must agree with the one-shot.
void test_sha256_incremental_matches_oneshot() {
    const char* msg = "the quick brown fox jumps over the lazy dog, twice, at length";
    uint8_t a[kSha256DigestSize];
    sha256(reinterpret_cast<const uint8_t*>(msg), std::strlen(msg), a);

    Sha256 h;
    for (size_t i = 0; i < std::strlen(msg); i++) {
        h.update(reinterpret_cast<const uint8_t*>(msg) + i, 1);
    }
    uint8_t b[kSha256DigestSize];
    h.finish(b);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(a, b, kSha256DigestSize);
}

// ---- CCM, RFC 3610 packet vectors -------------------------------------------

static void rfc3610Case(const char* key_hex, const char* nonce_hex, const char* input_hex,
                        size_t aad_len, size_t tag_len, const char* expect_hex) {
    auto key = hexToBytes(key_hex);
    auto nonce = hexToBytes(nonce_hex);
    auto input = hexToBytes(input_hex);
    auto expect = hexToBytes(expect_hex);

    Aes128 aes;
    aes.setKey(key.data());

    std::vector<uint8_t> data(input.begin() + aad_len, input.end());
    std::vector<uint8_t> tag(tag_len, 0);
    TEST_ASSERT_TRUE(ccmEncrypt(aes, nonce.data(), nonce.size(), input.data(), aad_len,
                                data.data(), data.size(), tag.data(), tag_len));

    // The RFC prints AAD || ciphertext || tag as one packet.
    std::vector<uint8_t> got(input.begin(), input.begin() + aad_len);
    got.insert(got.end(), data.begin(), data.end());
    got.insert(got.end(), tag.begin(), tag.end());
    TEST_ASSERT_EQUAL_size_t(expect.size(), got.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expect.data(), got.data(), expect.size());

    // And it must decrypt back to the plaintext it came from.
    TEST_ASSERT_TRUE(ccmDecrypt(aes, nonce.data(), nonce.size(), input.data(), aad_len,
                                data.data(), data.size(), tag.data(), tag_len));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(input.data() + aad_len, data.data(), data.size());
}

void test_ccm_rfc3610_vector_1() {
    rfc3610Case("C0C1C2C3C4C5C6C7C8C9CACBCCCDCECF", "00000003020100A0A1A2A3A4A5",
                "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E",
                /*aad_len=*/8, /*tag_len=*/8,
                "0001020304050607588C979A61C663D2F066D0C2C0F989806D5F6B61DAC384"
                "17E8D12CFDF926E0");
}

void test_ccm_rfc3610_vector_2() {
    rfc3610Case("C0C1C2C3C4C5C6C7C8C9CACBCCCDCECF", "00000004030201A0A1A2A3A4A5",
                "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F",
                /*aad_len=*/8, /*tag_len=*/8,
                "000102030405060772C91A36E135F8CF291CA894085C87E3CC15C439C9E43A3B"
                "A091D56E10400916");
}

void test_ccm_rfc3610_vector_3() {
    rfc3610Case("C0C1C2C3C4C5C6C7C8C9CACBCCCDCECF", "00000005040302A0A1A2A3A4A5",
                "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F20",
                /*aad_len=*/8, /*tag_len=*/8,
                "000102030405060751B1E5F44A197D1DA46B0F8E2D282AE871E838BB64DA8596"
                "574ADAA76FBD9FB0C5");
}

// RFC 3610 packet vector #7: a 10-byte tag, which changes the M field of the B0
// flags byte and how far the masked MAC is truncated.
void test_ccm_rfc3610_vector_7_long_tag() {
    rfc3610Case("C0C1C2C3C4C5C6C7C8C9CACBCCCDCECF", "00000009080706A0A1A2A3A4A5",
                "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E",
                /*aad_len=*/8, /*tag_len=*/10,
                "00010203040506070135D1B2C95F41D5D1D4FEC185D166B8094E999DFED96C04"
                "8C56602C97ACBB7490");
}

// The published vectors all carry 23 to 25 payload bytes. Sweep the lengths
// either side of a block boundary too, because the zero-padding of the final
// CBC-MAC block is the easiest part of CCM to get wrong and the only part those
// vectors barely exercise.
void test_ccm_payload_length_boundaries_round_trip() {
    auto key = hexToBytes("C0C1C2C3C4C5C6C7C8C9CACBCCCDCECF");
    auto nonce = hexToBytes("00000009080706A0A1A2A3A4A5");
    Aes128 aes;
    aes.setKey(key.data());

    const size_t lengths[] = {0, 1, 15, 16, 17, 31, 32, 33};
    for (size_t len : lengths) {
        const uint8_t aad[8] = {0, 1, 2, 3, 4, 5, 6, 7};
        uint8_t plain[33];
        uint8_t data[33];
        for (size_t i = 0; i < len; i++) {
            plain[i] = static_cast<uint8_t>(i * 7 + 1);
            data[i] = plain[i];
        }
        uint8_t tag[kFrameTagLen] = {0};
        TEST_ASSERT_TRUE(
            ccmEncrypt(aes, nonce.data(), nonce.size(), aad, 8, data, len, tag, kFrameTagLen));
        TEST_ASSERT_TRUE(
            ccmDecrypt(aes, nonce.data(), nonce.size(), aad, 8, data, len, tag, kFrameTagLen));
        if (len > 0) {
            TEST_ASSERT_EQUAL_UINT8_ARRAY(plain, data, len);
        }
    }
}

// Tag lengths other than 8 only change the M field of the B0 flags byte and how
// far the masked MAC is truncated. The published vectors above pin the
// algorithm itself; this covers the remaining lengths by construction, since a
// wrong M would still round-trip against itself while silently authenticating
// with fewer bits than claimed.
void test_ccm_other_tag_lengths_round_trip_and_detect_tampering() {
    auto key = hexToBytes("C0C1C2C3C4C5C6C7C8C9CACBCCCDCECF");
    auto nonce = hexToBytes("00000009080706A0A1A2A3A4A5");
    Aes128 aes;
    aes.setKey(key.data());

    for (size_t tag_len = 4; tag_len <= 16; tag_len += 2) {
        const uint8_t aad[8] = {0, 1, 2, 3, 4, 5, 6, 7};
        uint8_t plain[23];
        for (uint8_t i = 0; i < sizeof(plain); i++) {
            plain[i] = static_cast<uint8_t>(8 + i);
        }
        uint8_t data[23];
        std::memcpy(data, plain, sizeof(plain));
        uint8_t tag[16] = {0};

        TEST_ASSERT_TRUE(ccmEncrypt(aes, nonce.data(), nonce.size(), aad, 8, data, sizeof(data),
                                    tag, tag_len));
        // Encryption must actually have changed the payload.
        TEST_ASSERT_NOT_EQUAL(0, std::memcmp(plain, data, sizeof(data)));
        TEST_ASSERT_TRUE(ccmDecrypt(aes, nonce.data(), nonce.size(), aad, 8, data, sizeof(data),
                                    tag, tag_len));
        TEST_ASSERT_EQUAL_UINT8_ARRAY(plain, data, sizeof(data));

        // Every byte of the tag is load-bearing at this length.
        for (size_t i = 0; i < tag_len; i++) {
            uint8_t scratch[23];
            std::memcpy(scratch, plain, sizeof(scratch));
            uint8_t fresh_tag[16] = {0};
            TEST_ASSERT_TRUE(ccmEncrypt(aes, nonce.data(), nonce.size(), aad, 8, scratch,
                                        sizeof(scratch), fresh_tag, tag_len));
            fresh_tag[i] ^= 0x80;
            TEST_ASSERT_FALSE(ccmDecrypt(aes, nonce.data(), nonce.size(), aad, 8, scratch,
                                         sizeof(scratch), fresh_tag, tag_len));
        }
    }
}

void test_ccm_rejects_bad_parameters() {
    Aes128 aes;
    uint8_t key[16] = {0};
    aes.setKey(key);
    uint8_t nonce[13] = {0};
    uint8_t data[8] = {0};
    uint8_t tag[8] = {0};
    // Odd tag length, too-short tag, and an out-of-range nonce.
    TEST_ASSERT_FALSE(ccmEncrypt(aes, nonce, 13, nullptr, 0, data, 8, tag, 7));
    TEST_ASSERT_FALSE(ccmEncrypt(aes, nonce, 13, nullptr, 0, data, 8, tag, 2));
    TEST_ASSERT_FALSE(ccmEncrypt(aes, nonce, 6, nullptr, 0, data, 8, tag, 8));
    TEST_ASSERT_FALSE(ccmEncrypt(aes, nonce, 14, nullptr, 0, data, 8, tag, 8));
}

void test_ccm_tamper_is_detected_everywhere() {
    auto key = hexToBytes("C0C1C2C3C4C5C6C7C8C9CACBCCCDCECF");
    auto nonce = hexToBytes("00000003020100A0A1A2A3A4A5");
    Aes128 aes;
    aes.setKey(key.data());

    const uint8_t aad[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    uint8_t original[16];
    for (uint8_t i = 0; i < 16; i++) {
        original[i] = i;
    }

    // Flip one bit of the ciphertext, then one of the AAD, then one of the tag;
    // all three must fail.
    for (int where = 0; where < 3; where++) {
        uint8_t data[16];
        std::memcpy(data, original, 16);
        uint8_t tag[6];
        uint8_t aad_copy[8];
        std::memcpy(aad_copy, aad, 8);
        TEST_ASSERT_TRUE(ccmEncrypt(aes, nonce.data(), nonce.size(), aad_copy, 8, data, 16, tag, 6));

        if (where == 0) data[3] ^= 0x01;
        if (where == 1) aad_copy[2] ^= 0x01;
        if (where == 2) tag[5] ^= 0x01;

        TEST_ASSERT_FALSE(
            ccmDecrypt(aes, nonce.data(), nonce.size(), aad_copy, 8, data, 16, tag, 6));
    }
}

// ---- Key derivation ----------------------------------------------------------

void test_group_key_is_first_half_of_passphrase_sha256() {
    uint8_t digest[kSha256DigestSize];
    sha256(reinterpret_cast<const uint8_t*>("formation"), 9, digest);
    uint8_t key[kAesKeySize];
    deriveGroupKey("formation", key);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(digest, key, kAesKeySize);
}

void test_different_passphrases_give_different_keys() {
    uint8_t a[kAesKeySize];
    uint8_t b[kAesKeySize];
    deriveGroupKey("alpha", a);
    deriveGroupKey("alphb", b);
    TEST_ASSERT_NOT_EQUAL(0, std::memcmp(a, b, kAesKeySize));
}

void test_empty_and_null_passphrase_agree() {
    uint8_t a[kAesKeySize];
    uint8_t b[kAesKeySize];
    deriveGroupKey("", a);
    deriveGroupKey(nullptr, b);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(a, b, kAesKeySize);
}

// ---- The frame cipher --------------------------------------------------------

static size_t makePositionPacket(uint32_t uid, uint8_t* buf, size_t cap) {
    PositionPacket p{};
    p.uid = uid;
    p.lat = 370000000;
    p.lon = -1220000000;
    p.alt_m = 120;
    p.speed_cms = 1500;
    p.course_ddeg = 900;
    p.flags = POSITION_FLAG_HAS_FIX;
    return encodePosition(p, buf, cap);
}

void test_frame_round_trips_and_hides_the_payload() {
    uint8_t key[kAesKeySize];
    deriveGroupKey("hangar", key);

    CcmCrypto tx;
    tx.begin(0xAABBCCDD, key, 0);
    CcmCrypto rx;
    rx.begin(0x11223344, key, 0);

    uint8_t buf[64];
    const size_t plain_len = makePositionPacket(0xAABBCCDD, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(kPositionPacketSize, plain_len);
    uint8_t plain_copy[64];
    std::memcpy(plain_copy, buf, plain_len);

    const size_t frame_len = tx.encrypt(buf, plain_len, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(kPositionFrameSize, frame_len);

    // Header is readable, payload is not.
    TEST_ASSERT_EQUAL_UINT8(kProtocolVersion, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PacketType::Position), buf[1]);
    TEST_ASSERT_NOT_EQUAL(0, std::memcmp(buf + kFrameHeaderLen, plain_copy + kHeaderSize,
                                         plain_len - kHeaderSize));

    size_t out_len = 0;
    TEST_ASSERT_TRUE(rx.decrypt(buf, frame_len, out_len, 1000));
    TEST_ASSERT_EQUAL_size_t(plain_len, out_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(plain_copy, buf, plain_len);

    PositionPacket decoded;
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeResult::Ok),
                      static_cast<int>(decodePosition(buf, out_len, decoded)));
    TEST_ASSERT_EQUAL_INT32(370000000, decoded.lat);
    TEST_ASSERT_EQUAL_UINT32(0xAABBCCDD, decoded.uid);
}

void test_wrong_group_passphrase_is_rejected() {
    uint8_t key_a[kAesKeySize];
    uint8_t key_b[kAesKeySize];
    deriveGroupKey("ours", key_a);
    deriveGroupKey("theirs", key_b);

    CcmCrypto tx;
    tx.begin(1, key_a, 0);
    CcmCrypto rx;
    rx.begin(2, key_b, 0);

    uint8_t buf[64];
    const size_t plain_len = makePositionPacket(1, buf, sizeof(buf));
    const size_t frame_len = tx.encrypt(buf, plain_len, sizeof(buf));

    size_t out_len = 0;
    TEST_ASSERT_FALSE(rx.decrypt(buf, frame_len, out_len, 1000));
    TEST_ASSERT_EQUAL_UINT32(1, rx.badTagCount());
    TEST_ASSERT_FALSE(rx.lastRejectWasReplay());
}

void test_flipping_any_clear_header_byte_is_rejected() {
    uint8_t key[kAesKeySize];
    deriveGroupKey("hangar", key);

    for (size_t i = 0; i < kFrameHeaderLen; i++) {
        CcmCrypto tx;
        tx.begin(0x1234, key, 0);
        CcmCrypto rx;
        rx.begin(0x5678, key, 0);

        uint8_t buf[64];
        const size_t plain_len = makePositionPacket(0x1234, buf, sizeof(buf));
        const size_t frame_len = tx.encrypt(buf, plain_len, sizeof(buf));
        buf[i] ^= 0x01;  // the header is AAD, so this must invalidate the tag

        size_t out_len = 0;
        TEST_ASSERT_FALSE(rx.decrypt(buf, frame_len, out_len, 1000));
    }
}

void test_replayed_frame_is_rejected() {
    uint8_t key[kAesKeySize];
    deriveGroupKey("hangar", key);

    CcmCrypto tx;
    tx.begin(0x1234, key, 0);
    CcmCrypto rx;
    rx.begin(0x5678, key, 0);

    uint8_t original[64];
    const size_t plain_len = makePositionPacket(0x1234, original, sizeof(original));
    const size_t frame_len = tx.encrypt(original, plain_len, sizeof(original));

    uint8_t first[64];
    std::memcpy(first, original, frame_len);
    size_t out_len = 0;
    TEST_ASSERT_TRUE(rx.decrypt(first, frame_len, out_len, 1000));

    // The identical frame, captured off the air and re-injected.
    uint8_t replay[64];
    std::memcpy(replay, original, frame_len);
    TEST_ASSERT_FALSE(rx.decrypt(replay, frame_len, out_len, 1100));
    TEST_ASSERT_EQUAL_UINT32(1, rx.replayCount());
    TEST_ASSERT_TRUE(rx.lastRejectWasReplay());
}

void test_counter_must_advance_within_a_session() {
    uint8_t key[kAesKeySize];
    deriveGroupKey("hangar", key);

    CcmCrypto tx;
    tx.begin(0x1234, key, 0);
    CcmCrypto rx;
    rx.begin(0x5678, key, 0);

    // Capture three consecutive frames, then deliver them out of order.
    uint8_t frames[3][64];
    size_t lens[3];
    for (int i = 0; i < 3; i++) {
        const size_t plain_len = makePositionPacket(0x1234, frames[i], sizeof(frames[i]));
        lens[i] = tx.encrypt(frames[i], plain_len, sizeof(frames[i]));
    }

    size_t out_len = 0;
    uint8_t scratch[64];
    std::memcpy(scratch, frames[2], lens[2]);
    TEST_ASSERT_TRUE(rx.decrypt(scratch, lens[2], out_len, 1000));  // newest accepted
    std::memcpy(scratch, frames[0], lens[0]);
    TEST_ASSERT_FALSE(rx.decrypt(scratch, lens[0], out_len, 1010));  // older rejected
    std::memcpy(scratch, frames[1], lens[1]);
    TEST_ASSERT_FALSE(rx.decrypt(scratch, lens[1], out_len, 1020));
}

void test_rebooted_peer_resyncs_after_the_quiet_window() {
    uint8_t key[kAesKeySize];
    deriveGroupKey("hangar", key);

    CcmCrypto rx;
    rx.begin(0x5678, key, 0);

    // A node beacons for a while...
    CcmCrypto tx;
    tx.begin(0x1234, key, 0);
    uint8_t buf[64];
    size_t out_len = 0;
    for (uint32_t t = 1000; t < 1500; t += 100) {
        const size_t plain_len = makePositionPacket(0x1234, buf, sizeof(buf));
        const size_t frame_len = tx.encrypt(buf, plain_len, sizeof(buf));
        TEST_ASSERT_TRUE(rx.decrypt(buf, frame_len, out_len, t));
    }

    // ...then reboots, restarting its counter from scratch. Inside the quiet
    // window that looks exactly like a replay and is refused.
    CcmCrypto rebooted;
    rebooted.begin(0x1234, key, 0);
    size_t plain_len = makePositionPacket(0x1234, buf, sizeof(buf));
    size_t frame_len = rebooted.encrypt(buf, plain_len, sizeof(buf));
    TEST_ASSERT_FALSE(rx.decrypt(buf, frame_len, out_len, 1600));

    // Past the window, the restart is accepted so the aircraft can rejoin.
    plain_len = makePositionPacket(0x1234, buf, sizeof(buf));
    frame_len = rebooted.encrypt(buf, plain_len, sizeof(buf));
    TEST_ASSERT_TRUE(rx.decrypt(buf, frame_len, out_len, 1400 + kReplayResyncMs + 1));
}

void test_two_senders_do_not_share_a_counter() {
    uint8_t key[kAesKeySize];
    deriveGroupKey("hangar", key);

    CcmCrypto rx;
    rx.begin(0x9999, key, 0);
    CcmCrypto a;
    a.begin(0xAAAA, key, 0);
    CcmCrypto b;
    b.begin(0xBBBB, key, 0);

    uint8_t buf[64];
    size_t out_len = 0;

    // A gets well ahead of B. B's low counter must still be accepted: the
    // sequence is per sender, not global.
    for (int i = 0; i < 5; i++) {
        const size_t plain_len = makePositionPacket(0xAAAA, buf, sizeof(buf));
        const size_t frame_len = a.encrypt(buf, plain_len, sizeof(buf));
        TEST_ASSERT_TRUE(rx.decrypt(buf, frame_len, out_len, 1000 + i * 10));
    }
    const size_t plain_len = makePositionPacket(0xBBBB, buf, sizeof(buf));
    const size_t frame_len = b.encrypt(buf, plain_len, sizeof(buf));
    TEST_ASSERT_TRUE(rx.decrypt(buf, frame_len, out_len, 1100));
}

void test_encrypt_refuses_a_buffer_without_room_for_the_tag() {
    uint8_t key[kAesKeySize];
    deriveGroupKey("hangar", key);
    CcmCrypto tx;
    tx.begin(1, key, 0);

    uint8_t buf[64];
    const size_t plain_len = makePositionPacket(1, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(0, tx.encrypt(buf, plain_len, plain_len + kFrameOverhead - 1));
}

void test_truncated_frame_is_rejected() {
    uint8_t key[kAesKeySize];
    deriveGroupKey("hangar", key);
    CcmCrypto tx;
    tx.begin(1, key, 0);
    CcmCrypto rx;
    rx.begin(2, key, 0);

    uint8_t buf[64];
    const size_t plain_len = makePositionPacket(1, buf, sizeof(buf));
    const size_t frame_len = tx.encrypt(buf, plain_len, sizeof(buf));

    size_t out_len = 0;
    TEST_ASSERT_FALSE(rx.decrypt(buf, frame_len - 1, out_len, 1000));
    TEST_ASSERT_FALSE(rx.decrypt(buf, kFrameHeaderLen, out_len, 1000));
    TEST_ASSERT_FALSE(rx.decrypt(buf, 0, out_len, 1000));
}

void test_announce_frame_fits_the_documented_size() {
    uint8_t key[kAesKeySize];
    deriveGroupKey("hangar", key);
    CcmCrypto tx;
    tx.begin(1, key, 0);

    AnnouncePacket a{};
    a.uid = 1;
    for (size_t i = 0; i < kMaxNameLen; i++) {
        a.name[i] = static_cast<char>('A' + (i % 26));
    }
    a.name[kMaxNameLen] = 0;
    a.capabilities = CAP_HAS_GPS | CAP_HAS_MSP_FC;

    uint8_t buf[64];
    const size_t plain_len = encodeAnnounce(a, buf, sizeof(buf));
    const size_t frame_len = tx.encrypt(buf, plain_len, sizeof(buf));
    TEST_ASSERT_TRUE(frame_len <= kAnnounceFrameSize);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_aes128_fips197_vector);
    RUN_TEST(test_aes128_fips197_appendix_b);
    RUN_TEST(test_aes128_rekey_is_clean);

    RUN_TEST(test_sha256_abc);
    RUN_TEST(test_sha256_empty);
    RUN_TEST(test_sha256_two_block);
    RUN_TEST(test_sha256_incremental_matches_oneshot);

    RUN_TEST(test_ccm_rfc3610_vector_1);
    RUN_TEST(test_ccm_rfc3610_vector_2);
    RUN_TEST(test_ccm_rfc3610_vector_3);
    RUN_TEST(test_ccm_rfc3610_vector_7_long_tag);
    RUN_TEST(test_ccm_payload_length_boundaries_round_trip);
    RUN_TEST(test_ccm_other_tag_lengths_round_trip_and_detect_tampering);
    RUN_TEST(test_ccm_rejects_bad_parameters);
    RUN_TEST(test_ccm_tamper_is_detected_everywhere);

    RUN_TEST(test_group_key_is_first_half_of_passphrase_sha256);
    RUN_TEST(test_different_passphrases_give_different_keys);
    RUN_TEST(test_empty_and_null_passphrase_agree);

    RUN_TEST(test_frame_round_trips_and_hides_the_payload);
    RUN_TEST(test_wrong_group_passphrase_is_rejected);
    RUN_TEST(test_flipping_any_clear_header_byte_is_rejected);
    RUN_TEST(test_replayed_frame_is_rejected);
    RUN_TEST(test_counter_must_advance_within_a_session);
    RUN_TEST(test_rebooted_peer_resyncs_after_the_quiet_window);
    RUN_TEST(test_two_senders_do_not_share_a_counter);
    RUN_TEST(test_encrypt_refuses_a_buffer_without_room_for_the_tag);
    RUN_TEST(test_truncated_frame_is_rejected);
    RUN_TEST(test_announce_frame_fits_the_documented_size);
    return UNITY_END();
}
