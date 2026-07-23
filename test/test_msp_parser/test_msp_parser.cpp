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

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_parses_valid_frame);
    RUN_TEST(test_zero_length_frame);
    RUN_TEST(test_bad_crc_rejected);
    RUN_TEST(test_leading_garbage_ignored);
    RUN_TEST(test_resync_after_bad_frame);
    RUN_TEST(test_two_frames_back_to_back);
    RUN_TEST(test_oversize_frame_dropped_but_resyncs);
    return UNITY_END();
}
