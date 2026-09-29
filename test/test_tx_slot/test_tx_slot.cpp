// The one-frame transmit slot.
//
// It exists because the node has two independent transmit schedules on one
// half-duplex radio - the per-radio ALOHA beacon and the node announce - and
// neither knows the other exists. Before this, the second frame of a collision
// was thrown away, which made a working radio report a steadily climbing
// transmit-drop count. See tx_slot.h.

#include <unity.h>

#include <cstring>

#include "tx_slot.h"

using namespace ff;

void setUp() {}
void tearDown() {}

static const uint8_t kFrameA[] = {0xAA, 0xBB, 0xCC};
static const uint8_t kFrameB[] = {0x11, 0x22};

void test_a_new_slot_is_empty() {
    TxSlot s;
    TEST_ASSERT_TRUE(s.empty());
    TEST_ASSERT_EQUAL_size_t(0, s.size());
    TEST_ASSERT_EQUAL_UINT32(0, s.deferred());
}

void test_a_held_frame_comes_back_byte_for_byte() {
    TxSlot s;
    TEST_ASSERT_TRUE(s.push(kFrameA, sizeof(kFrameA)));
    TEST_ASSERT_FALSE(s.empty());
    TEST_ASSERT_EQUAL_size_t(sizeof(kFrameA), s.size());
    TEST_ASSERT_EQUAL_MEMORY(kFrameA, s.data(), sizeof(kFrameA));
}

// The slot copies. The caller's buffer is a stack temporary in the driver, and
// the frame has to survive until the radio goes idle a whole airtime later.
void test_the_frame_is_copied_not_referenced() {
    TxSlot s;
    uint8_t scratch[3] = {1, 2, 3};
    TEST_ASSERT_TRUE(s.push(scratch, sizeof(scratch)));
    std::memset(scratch, 0, sizeof(scratch));
    const uint8_t expect[3] = {1, 2, 3};
    TEST_ASSERT_EQUAL_MEMORY(expect, s.data(), sizeof(expect));
}

// The frame already waiting is the older of the two. Overwriting it would drop
// it with nothing counting the loss AND put the two frames on the air out of
// order, so the newer one is refused instead.
void test_a_second_frame_is_refused_and_never_overwrites_the_first() {
    TxSlot s;
    TEST_ASSERT_TRUE(s.push(kFrameA, sizeof(kFrameA)));
    TEST_ASSERT_FALSE(s.push(kFrameB, sizeof(kFrameB)));
    TEST_ASSERT_EQUAL_size_t(sizeof(kFrameA), s.size());
    TEST_ASSERT_EQUAL_MEMORY(kFrameA, s.data(), sizeof(kFrameA));
}

void test_clearing_makes_room_for_the_next_one() {
    TxSlot s;
    TEST_ASSERT_TRUE(s.push(kFrameA, sizeof(kFrameA)));
    s.clear();
    TEST_ASSERT_TRUE(s.empty());
    TEST_ASSERT_EQUAL_size_t(0, s.size());
    TEST_ASSERT_TRUE(s.push(kFrameB, sizeof(kFrameB)));
    TEST_ASSERT_EQUAL_MEMORY(kFrameB, s.data(), sizeof(kFrameB));
}

// Only successful holds count. A refused frame is the driver's drop to record,
// and counting it here too would double-count the same lost frame.
void test_only_accepted_frames_count_as_deferred() {
    TxSlot s;
    TEST_ASSERT_TRUE(s.push(kFrameA, sizeof(kFrameA)));
    TEST_ASSERT_FALSE(s.push(kFrameB, sizeof(kFrameB)));
    TEST_ASSERT_EQUAL_UINT32(1, s.deferred());
    s.clear();
    TEST_ASSERT_TRUE(s.push(kFrameB, sizeof(kFrameB)));
    TEST_ASSERT_EQUAL_UINT32(2, s.deferred());
}

// The count is "since boot" and survives the slot emptying, exactly like the
// log ring's warning and error counters.
void test_the_deferred_count_survives_a_clear() {
    TxSlot s;
    TEST_ASSERT_TRUE(s.push(kFrameA, sizeof(kFrameA)));
    s.clear();
    TEST_ASSERT_EQUAL_UINT32(1, s.deferred());
}

void test_an_oversize_frame_is_refused_rather_than_truncated() {
    TxSlot s;
    uint8_t huge[TxSlot::kCapacity + 1];
    std::memset(huge, 0x5A, sizeof(huge));
    TEST_ASSERT_FALSE(s.push(huge, sizeof(huge)));
    TEST_ASSERT_TRUE(s.empty());
    TEST_ASSERT_EQUAL_UINT32(0, s.deferred());
}

void test_a_frame_exactly_at_capacity_fits() {
    TxSlot s;
    uint8_t full[TxSlot::kCapacity];
    std::memset(full, 0x5A, sizeof(full));
    TEST_ASSERT_TRUE(s.push(full, sizeof(full)));
    TEST_ASSERT_EQUAL_size_t(TxSlot::kCapacity, s.size());
    TEST_ASSERT_EQUAL_MEMORY(full, s.data(), sizeof(full));
}

void test_an_empty_or_null_frame_is_refused() {
    TxSlot s;
    TEST_ASSERT_FALSE(s.push(kFrameA, 0));
    TEST_ASSERT_FALSE(s.push(nullptr, 4));
    TEST_ASSERT_TRUE(s.empty());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_a_new_slot_is_empty);
    RUN_TEST(test_a_held_frame_comes_back_byte_for_byte);
    RUN_TEST(test_the_frame_is_copied_not_referenced);
    RUN_TEST(test_a_second_frame_is_refused_and_never_overwrites_the_first);
    RUN_TEST(test_clearing_makes_room_for_the_next_one);
    RUN_TEST(test_only_accepted_frames_count_as_deferred);
    RUN_TEST(test_the_deferred_count_survives_a_clear);
    RUN_TEST(test_an_oversize_frame_is_refused_rather_than_truncated);
    RUN_TEST(test_a_frame_exactly_at_capacity_fits);
    RUN_TEST(test_an_empty_or_null_frame_is_refused);
    return UNITY_END();
}
