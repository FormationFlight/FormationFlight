#include <unity.h>

#include "ring_buffer.h"

using namespace ff;

void setUp() {}
void tearDown() {}

void test_empty_pop_fails() {
    SpscRing<int, 4> r;
    TEST_ASSERT_TRUE(r.empty());
    int v = 123;
    TEST_ASSERT_FALSE(r.pop(v));
    TEST_ASSERT_EQUAL_INT(123, v);  // untouched
}

void test_push_pop_fifo_order() {
    SpscRing<int, 8> r;
    for (int i = 1; i <= 5; i++) {
        TEST_ASSERT_TRUE(r.push(i));
    }
    TEST_ASSERT_EQUAL_UINT32(5, r.size());
    for (int i = 1; i <= 5; i++) {
        int v = 0;
        TEST_ASSERT_TRUE(r.pop(v));
        TEST_ASSERT_EQUAL_INT(i, v);
    }
    TEST_ASSERT_TRUE(r.empty());
}

void test_full_drops_and_counts() {
    SpscRing<int, 4> r;  // usable capacity 3
    TEST_ASSERT_TRUE(r.push(1));
    TEST_ASSERT_TRUE(r.push(2));
    TEST_ASSERT_TRUE(r.push(3));
    TEST_ASSERT_TRUE(r.full());
    TEST_ASSERT_FALSE(r.push(4));  // dropped
    TEST_ASSERT_FALSE(r.push(5));  // dropped
    TEST_ASSERT_EQUAL_UINT32(2, r.dropped());
    // Existing items intact and in order.
    int v = 0;
    TEST_ASSERT_TRUE(r.pop(v));
    TEST_ASSERT_EQUAL_INT(1, v);
}

void test_wraps_around_indices() {
    SpscRing<int, 4> r;  // usable 3
    // Push 2 / pop 2 per round (net zero, peak size 2) so we never overflow, but
    // the head/tail indices wrap around the 4-slot buffer many times.
    int expected = 0;
    int next_push = 0;
    for (int round = 0; round < 10; round++) {
        TEST_ASSERT_TRUE(r.push(next_push++));
        TEST_ASSERT_TRUE(r.push(next_push++));
        int v = 0;
        TEST_ASSERT_TRUE(r.pop(v));
        TEST_ASSERT_EQUAL_INT(expected++, v);
        TEST_ASSERT_TRUE(r.pop(v));
        TEST_ASSERT_EQUAL_INT(expected++, v);
    }
    TEST_ASSERT_TRUE(r.empty());
    TEST_ASSERT_EQUAL_INT(20, next_push);
    TEST_ASSERT_EQUAL_INT(20, expected);
}

struct Frame {
    uint32_t ts;
    uint8_t len;
    uint8_t data[8];
};

void test_struct_payload_roundtrip() {
    SpscRing<Frame, 4> r;
    Frame in{};
    in.ts = 0xCAFEBABE;
    in.len = 3;
    in.data[0] = 0xDE;
    in.data[1] = 0xAD;
    in.data[2] = 0xBE;
    TEST_ASSERT_TRUE(r.push(in));
    Frame out{};
    TEST_ASSERT_TRUE(r.pop(out));
    TEST_ASSERT_EQUAL_UINT32(0xCAFEBABE, out.ts);
    TEST_ASSERT_EQUAL_UINT8(3, out.len);
    TEST_ASSERT_EQUAL_UINT8(0xDE, out.data[0]);
    TEST_ASSERT_EQUAL_UINT8(0xAD, out.data[1]);
    TEST_ASSERT_EQUAL_UINT8(0xBE, out.data[2]);
}

void test_interleaved_push_pop_size() {
    SpscRing<int, 8> r;
    TEST_ASSERT_EQUAL_UINT32(0, r.size());
    r.push(1);
    r.push(2);
    TEST_ASSERT_EQUAL_UINT32(2, r.size());
    int v = 0;
    r.pop(v);
    TEST_ASSERT_EQUAL_UINT32(1, r.size());
    r.push(3);
    r.push(4);
    TEST_ASSERT_EQUAL_UINT32(3, r.size());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_pop_fails);
    RUN_TEST(test_push_pop_fifo_order);
    RUN_TEST(test_full_drops_and_counts);
    RUN_TEST(test_wraps_around_indices);
    RUN_TEST(test_struct_payload_roundtrip);
    RUN_TEST(test_interleaved_push_pop_size);
    return UNITY_END();
}
