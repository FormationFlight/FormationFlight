#include <unity.h>

#include <vector>

#include "msp_parser.h"

using namespace ff;

void setUp() {}
void tearDown() {}

// Build a well-formed MSP v1 response frame.
static std::vector<uint8_t> frame(uint8_t id, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> f = {'$', 'M', '>'};
    uint8_t size = static_cast<uint8_t>(payload.size());
    f.push_back(size);
    f.push_back(id);
    uint8_t crc = size ^ id;
    for (uint8_t b : payload) {
        f.push_back(b);
        crc ^= b;
    }
    f.push_back(crc);
    return f;
}

// Feed a whole buffer; return the count of completed frames and remember the last.
static int feedAll(MspParser& p, const std::vector<uint8_t>& bytes, MspParser* last = nullptr) {
    int frames = 0;
    for (uint8_t b : bytes) {
        if (p.feed(b)) {
            frames++;
            if (last) *last = p;
        }
    }
    return frames;
}

void test_parses_valid_frame() {
    MspParser p;
    std::vector<uint8_t> payload = {1, 2, 3, 4, 5};
    auto f = frame(106, payload);

    int frames = 0;
    for (size_t i = 0; i < f.size(); i++) {
        bool done = p.feed(f[i]);
        if (i + 1 < f.size()) {
            TEST_ASSERT_FALSE(done);  // not complete until the crc byte
        } else {
            TEST_ASSERT_TRUE(done);
            frames = 1;
        }
    }
    TEST_ASSERT_EQUAL_INT(1, frames);
    TEST_ASSERT_EQUAL_UINT8(106, p.id());
    TEST_ASSERT_EQUAL_UINT8(5, p.size());
    for (uint8_t i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_UINT8(payload[i], p.payload()[i]);
    }
}

void test_zero_length_frame() {
    MspParser p;
    auto f = frame(42, {});  // ack-style, no payload
    TEST_ASSERT_EQUAL_INT(1, feedAll(p, f));
    TEST_ASSERT_EQUAL_UINT8(42, p.id());
    TEST_ASSERT_EQUAL_UINT8(0, p.size());
}

void test_bad_crc_rejected() {
    MspParser p;
    auto f = frame(106, {1, 2, 3});
    f.back() ^= 0xFF;  // corrupt crc
    TEST_ASSERT_EQUAL_INT(0, feedAll(p, f));
}

void test_leading_garbage_ignored() {
    MspParser p;
    std::vector<uint8_t> stream = {0x00, 0xFF, 'x', 'M', '>'};  // noise
    auto f = frame(7, {9, 9});
    stream.insert(stream.end(), f.begin(), f.end());
    TEST_ASSERT_EQUAL_INT(1, feedAll(p, stream));
    TEST_ASSERT_EQUAL_UINT8(7, p.id());
}

void test_resync_after_bad_frame() {
    MspParser p;
    // A complete but corrupt (bad-crc) frame resets the parser cleanly; the good
    // frame that follows must still parse.
    auto bad = frame(100, {1, 2, 3});
    bad.back() ^= 0xFF;  // corrupt crc
    auto good = frame(20, {0xAB});
    std::vector<uint8_t> stream(bad);
    stream.insert(stream.end(), good.begin(), good.end());
    TEST_ASSERT_EQUAL_INT(1, feedAll(p, stream));
    TEST_ASSERT_EQUAL_UINT8(20, p.id());
    TEST_ASSERT_EQUAL_UINT8(0xAB, p.payload()[0]);
}

void test_two_frames_back_to_back() {
    MspParser p;
    auto f1 = frame(1, {10});
    auto f2 = frame(2, {20, 30});
    std::vector<uint8_t> stream(f1);
    stream.insert(stream.end(), f2.begin(), f2.end());
    TEST_ASSERT_EQUAL_INT(2, feedAll(p, stream));
    // After the last frame, the parser reflects the second one.
    TEST_ASSERT_EQUAL_UINT8(2, p.id());
    TEST_ASSERT_EQUAL_UINT8(2, p.size());
}

void test_oversize_frame_dropped_but_resyncs() {
    MspParser p;
    // Declared size larger than the parser buffer: must be dropped, then a normal
    // frame after it still parses.
    std::vector<uint8_t> big = {'$', 'M', '>', static_cast<uint8_t>(kMspMaxPayload + 5)};
    big.push_back(50);  // id
    uint8_t crc = static_cast<uint8_t>(kMspMaxPayload + 5) ^ 50;
    for (uint8_t i = 0; i < kMspMaxPayload + 5; i++) {
        big.push_back(i);
        crc ^= i;
    }
    big.push_back(crc);  // valid crc, but oversize -> dropped
    auto good = frame(60, {1});
    big.insert(big.end(), good.begin(), good.end());

    MspParser p2;
    int frames = feedAll(p2, big);
    TEST_ASSERT_EQUAL_INT(1, frames);  // only the good one
    TEST_ASSERT_EQUAL_UINT8(60, p2.id());
}

// ---- MSP v2 framing -----------------------------------------------------------

#include "msp_crc.h"

static std::vector<uint8_t> frame2(uint16_t id, const std::vector<uint8_t>& payload,
                                   uint8_t flag = 0) {
    std::vector<uint8_t> f = {'$', 'X', '>'};
    const uint16_t size = static_cast<uint16_t>(payload.size());
    const uint8_t hdr[] = {flag, static_cast<uint8_t>(id & 0xFF), static_cast<uint8_t>(id >> 8),
                           static_cast<uint8_t>(size & 0xFF), static_cast<uint8_t>(size >> 8)};
    uint8_t crc = 0;
    for (uint8_t b : hdr) {
        f.push_back(b);
        crc = mspCrc8DvbS2(crc, b);
    }
    for (uint8_t b : payload) {
        f.push_back(b);
        crc = mspCrc8DvbS2(crc, b);
    }
    f.push_back(crc);
    return f;
}

void test_v2_parses_valid_frame() {
    MspParser p;
    std::vector<uint8_t> payload = {9, 8, 7};
    auto f = frame2(0x2010, payload, 0);
    MspParser last;
    TEST_ASSERT_EQUAL_INT(1, feedAll(p, f, &last));
    TEST_ASSERT_EQUAL_UINT8(2, last.version());
    TEST_ASSERT_EQUAL_UINT16(0x2010, last.id());
    TEST_ASSERT_EQUAL_UINT16(3, last.size());
    TEST_ASSERT_EQUAL_UINT8(0, last.flag());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload.data(), last.payload(), 3);
}

void test_v2_zero_length_frame() {
    MspParser p;
    auto f = frame2(0x2000, {});
    MspParser last;
    TEST_ASSERT_EQUAL_INT(1, feedAll(p, f, &last));
    TEST_ASSERT_EQUAL_UINT16(0x2000, last.id());
    TEST_ASSERT_EQUAL_UINT16(0, last.size());
}

void test_v2_bad_crc_rejected() {
    MspParser p;
    auto f = frame2(0x2010, {1, 2, 3});
    f.back() ^= 0x01;
    TEST_ASSERT_EQUAL_INT(0, feedAll(p, f));
}

void test_v1_and_v2_frames_interleave() {
    MspParser p;
    auto a = frame(106, {1, 2});
    auto b = frame2(0x2010, {3, 4, 5});
    auto c = frame(101, {6});
    std::vector<uint8_t> all;
    all.insert(all.end(), a.begin(), a.end());
    all.insert(all.end(), b.begin(), b.end());
    all.insert(all.end(), c.begin(), c.end());
    int frames = 0;
    std::vector<uint16_t> ids;
    std::vector<uint8_t> versions;
    for (uint8_t byte : all) {
        if (p.feed(byte)) {
            frames++;
            ids.push_back(p.id());
            versions.push_back(p.version());
        }
    }
    TEST_ASSERT_EQUAL_INT(3, frames);
    TEST_ASSERT_EQUAL_UINT16(106, ids[0]);
    TEST_ASSERT_EQUAL_UINT16(0x2010, ids[1]);
    TEST_ASSERT_EQUAL_UINT16(101, ids[2]);
    TEST_ASSERT_EQUAL_UINT8(1, versions[0]);
    TEST_ASSERT_EQUAL_UINT8(2, versions[1]);
    TEST_ASSERT_EQUAL_UINT8(1, versions[2]);
}

void test_v2_oversize_frame_dropped_but_resyncs() {
    MspParser p;
    std::vector<uint8_t> big(kMspMaxPayload + 1, 0x5A);
    auto f = frame2(0x2010, big);
    TEST_ASSERT_EQUAL_INT(0, feedAll(p, f));
    auto ok = frame2(0x2000, {1});
    TEST_ASSERT_EQUAL_INT(1, feedAll(p, ok));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_parses_valid_frame);
    RUN_TEST(test_zero_length_frame);
    RUN_TEST(test_bad_crc_rejected);
    RUN_TEST(test_leading_garbage_ignored);
    RUN_TEST(test_resync_after_bad_frame);
    RUN_TEST(test_two_frames_back_to_back);
    RUN_TEST(test_oversize_frame_dropped_but_resyncs);
    RUN_TEST(test_v2_parses_valid_frame);
    RUN_TEST(test_v2_zero_length_frame);
    RUN_TEST(test_v2_bad_crc_rejected);
    RUN_TEST(test_v1_and_v2_frames_interleave);
    RUN_TEST(test_v2_oversize_frame_dropped_but_resyncs);
    return UNITY_END();
}
