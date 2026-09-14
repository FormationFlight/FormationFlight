// The rolling frame log.
//
// Small, but the indexing is the kind that is easy to get subtly wrong and hard
// to notice: at(0) must be the NEWEST entry, the ring must overwrite oldest
// first, and total() must keep counting past capacity so a polling client can
// tell it missed frames rather than silently believing the list is complete.

#include <unity.h>

#include "frame_log.h"

using namespace ff;

void setUp() {}
void tearDown() {}

static FrameLogEntry entry(uint32_t uid, uint32_t ms) {
    FrameLogEntry e;
    e.uid = uid;
    e.ms = ms;
    e.len = 31;
    e.rssi = -70;
    e.radio = 0;
    e.type = 1;
    e.result = FrameResult::Ok;
    return e;
}

void test_empty_log_reports_nothing() {
    FrameLog log;
    TEST_ASSERT_EQUAL_size_t(0, log.size());
    TEST_ASSERT_EQUAL_UINT32(0, log.total());
    TEST_ASSERT_EQUAL_size_t(kFrameLogCapacity, log.capacity());
}

void test_newest_entry_is_first() {
    FrameLog log;
    log.add(entry(1, 100));
    log.add(entry(2, 200));
    log.add(entry(3, 300));

    TEST_ASSERT_EQUAL_size_t(3, log.size());
    TEST_ASSERT_EQUAL_UINT32(3, log.at(0).uid);
    TEST_ASSERT_EQUAL_UINT32(2, log.at(1).uid);
    TEST_ASSERT_EQUAL_UINT32(1, log.at(2).uid);
}

void test_ring_overwrites_oldest_first() {
    FrameLog log;
    // One and a half times capacity, so every slot has been reused.
    const uint32_t n = kFrameLogCapacity + kFrameLogCapacity / 2;
    for (uint32_t i = 1; i <= n; i++) {
        log.add(entry(i, i * 10));
    }

    TEST_ASSERT_EQUAL_size_t(kFrameLogCapacity, log.size());
    TEST_ASSERT_EQUAL_UINT32(n, log.total());
    // Newest first, counting down, and the oldest survivor is n - capacity + 1.
    for (size_t i = 0; i < kFrameLogCapacity; i++) {
        TEST_ASSERT_EQUAL_UINT32(n - static_cast<uint32_t>(i), log.at(i).uid);
    }
}

// total() is what the web API's ?since= cursor is built on: it must keep
// counting even though only the last `capacity` entries are retained.
void test_total_counts_past_capacity() {
    FrameLog log;
    for (uint32_t i = 0; i < kFrameLogCapacity * 3; i++) {
        log.add(entry(i, i));
    }
    TEST_ASSERT_EQUAL_UINT32(kFrameLogCapacity * 3, log.total());
    TEST_ASSERT_EQUAL_size_t(kFrameLogCapacity, log.size());
}

void test_exactly_full_log_keeps_everything() {
    FrameLog log;
    for (uint32_t i = 1; i <= kFrameLogCapacity; i++) {
        log.add(entry(i, i));
    }
    TEST_ASSERT_EQUAL_size_t(kFrameLogCapacity, log.size());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kFrameLogCapacity), log.at(0).uid);
    TEST_ASSERT_EQUAL_UINT32(1, log.at(kFrameLogCapacity - 1).uid);
}

void test_clear_empties_but_keeps_the_running_total() {
    FrameLog log;
    for (uint32_t i = 0; i < 5; i++) {
        log.add(entry(i, i));
    }
    log.clear();
    TEST_ASSERT_EQUAL_size_t(0, log.size());
    // The sequence numbering must not restart, or a client's cursor would walk
    // backwards and it would replay frames it already had.
    TEST_ASSERT_EQUAL_UINT32(5, log.total());
}

void test_entry_fields_survive_the_round_trip() {
    FrameLog log;
    FrameLogEntry e;
    e.ms = 123456;
    e.uid = 0xDEADBEEF;
    e.len = 36;
    e.rssi = -104;
    e.radio = 2;
    e.type = 2;
    e.result = FrameResult::ReplayFail;
    log.add(e);

    const FrameLogEntry& got = log.at(0);
    TEST_ASSERT_EQUAL_UINT32(123456, got.ms);
    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEF, got.uid);
    TEST_ASSERT_EQUAL_UINT16(36, got.len);
    TEST_ASSERT_EQUAL_INT16(-104, got.rssi);
    TEST_ASSERT_EQUAL_UINT8(2, got.radio);
    TEST_ASSERT_EQUAL_UINT8(2, got.type);
    TEST_ASSERT_EQUAL(static_cast<int>(FrameResult::ReplayFail), static_cast<int>(got.result));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_log_reports_nothing);
    RUN_TEST(test_newest_entry_is_first);
    RUN_TEST(test_ring_overwrites_oldest_first);
    RUN_TEST(test_total_counts_past_capacity);
    RUN_TEST(test_exactly_full_log_keeps_everything);
    RUN_TEST(test_clear_empties_but_keeps_the_running_total);
    RUN_TEST(test_entry_fields_survive_the_round_trip);
    return UNITY_END();
}
