#include <unity.h>

#include "scheduler.h"

using namespace ff;

void setUp() {}
void tearDown() {}

// --- Test callbacks -------------------------------------------------------

struct Counter {
    int n = 0;
};
static void bump(void* ctx) { static_cast<Counter*>(ctx)->n++; }

struct CancelCtx {
    Scheduler* s;
    TimerHandle other;
    int n = 0;
};
static void cancelOther(void* ctx) {
    auto* c = static_cast<CancelCtx*>(ctx);
    c->n++;
    c->s->cancel(c->other);
}

struct RearmCtx {
    Scheduler* s;
    TimerHandle self;
    uint32_t next;
    int n = 0;
};
static void rearmSelf(void* ctx) {
    auto* c = static_cast<RearmCtx*>(ctx);
    c->n++;
    c->s->rearm(c->self, c->next);
}

// --- One-shot -------------------------------------------------------------

void test_oneshot_fires_once_at_due() {
    Scheduler s;
    Counter c;
    s.poll(0);
    s.after(100, bump, &c);
    s.poll(50);
    TEST_ASSERT_EQUAL_INT(0, c.n);  // not yet
    s.poll(100);
    TEST_ASSERT_EQUAL_INT(1, c.n);  // fired
    s.poll(200);
    TEST_ASSERT_EQUAL_INT(1, c.n);  // does not repeat
    TEST_ASSERT_EQUAL_UINT32(0, s.activeCount());
}

void test_oneshot_fires_when_polled_late() {
    Scheduler s;
    Counter c;
    s.poll(0);
    s.after(100, bump, &c);
    s.poll(5000);  // long after due
    TEST_ASSERT_EQUAL_INT(1, c.n);
}

// --- Periodic -------------------------------------------------------------

void test_periodic_fires_each_period() {
    Scheduler s;
    Counter c;
    s.poll(0);
    s.every(100, bump, &c);
    for (uint32_t t = 10; t <= 500; t += 10) {
        s.poll(t);
    }
    // Due at 100,200,300,400,500 -> 5 firings.
    TEST_ASSERT_EQUAL_INT(5, c.n);
}

void test_periodic_resyncs_after_long_stall_no_burst() {
    Scheduler s;
    Counter c;
    s.poll(0);
    s.every(100, bump, &c);
    // Jump far past many periods in a single poll: must fire once, not catch up
    // with a burst.
    s.poll(1000);
    TEST_ASSERT_EQUAL_INT(1, c.n);
    // Next firing is one period after the resync point (1000 -> 1100).
    s.poll(1099);
    TEST_ASSERT_EQUAL_INT(1, c.n);
    s.poll(1100);
    TEST_ASSERT_EQUAL_INT(2, c.n);
}

// --- Multiple timers ------------------------------------------------------

void test_multiple_timers_fire_in_one_poll() {
    Scheduler s;
    Counter a, b, d;
    s.poll(0);
    s.after(50, bump, &a);
    s.after(50, bump, &b);
    s.after(100, bump, &d);
    s.poll(50);
    TEST_ASSERT_EQUAL_INT(1, a.n);
    TEST_ASSERT_EQUAL_INT(1, b.n);
    TEST_ASSERT_EQUAL_INT(0, d.n);
}

// --- Cancellation ---------------------------------------------------------

void test_cancel_prevents_firing() {
    Scheduler s;
    Counter c;
    s.poll(0);
    TimerHandle h = s.after(100, bump, &c);
    s.cancel(h);
    s.poll(200);
    TEST_ASSERT_EQUAL_INT(0, c.n);
}

void test_cancel_from_callback() {
    // Two timers due in the same poll; the first cancels the second.
    Scheduler s;
    Counter victim;
    CancelCtx cc{&s, kInvalidTimer, 0};
    s.poll(0);
    TimerHandle hv = s.after(50, bump, &victim);
    cc.other = hv;
    s.after(50, cancelOther, &cc);
    s.poll(50);
    // Order of the two due timers isn't guaranteed, but the canceller must run
    // and the victim must not fire if cancelled before it runs. The canceller is
    // scheduled second (higher slot); slots fire in index order, so the victim
    // (lower slot) fires first here. Assert the robust invariant: canceller ran.
    TEST_ASSERT_EQUAL_INT(1, cc.n);
}

void test_cancel_from_callback_lower_slot_first() {
    // Canceller in a lower slot than its victim: victim must not fire.
    Scheduler s;
    Counter victim;
    CancelCtx cc{&s, kInvalidTimer, 0};
    s.poll(0);
    TimerHandle hc = s.after(50, cancelOther, &cc);  // slot 0
    (void)hc;
    TimerHandle hv = s.after(50, bump, &victim);     // slot 1
    cc.other = hv;
    s.poll(50);
    TEST_ASSERT_EQUAL_INT(1, cc.n);
    TEST_ASSERT_EQUAL_INT(0, victim.n);  // cancelled before it ran
}

// --- Re-arm (ALOHA beacon pattern) ---------------------------------------

void test_rearm_from_callback() {
    Scheduler s;
    RearmCtx rc{&s, kInvalidTimer, 100, 0};
    s.poll(0);
    rc.self = s.after(100, rearmSelf, &rc);
    s.poll(100);
    TEST_ASSERT_EQUAL_INT(1, rc.n);
    s.poll(199);
    TEST_ASSERT_EQUAL_INT(1, rc.n);
    s.poll(200);
    TEST_ASSERT_EQUAL_INT(2, rc.n);  // re-armed and fired again
}

// --- next-delay return ----------------------------------------------------

void test_poll_returns_time_to_next() {
    Scheduler s;
    Counter c;
    s.poll(0);
    s.after(100, bump, &c);
    s.after(250, bump, &c);
    uint32_t next = s.poll(40);
    TEST_ASSERT_EQUAL_UINT32(60, next);  // nearest due (100) - 40
}

void test_poll_returns_max_when_idle() {
    Scheduler s;
    uint32_t next = s.poll(10);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, next);
}

// --- Wraparound -----------------------------------------------------------

void test_deadline_survives_millis_wraparound() {
    Scheduler s;
    Counter c;
    s.poll(0xFFFFFF00u);
    s.after(0x200, bump, &c);  // due wraps past 0 to 0x100
    s.poll(0xFFFFFFF0u);       // before wrap: not yet due
    TEST_ASSERT_EQUAL_INT(0, c.n);
    s.poll(0x100u);            // after wrap: due
    TEST_ASSERT_EQUAL_INT(1, c.n);
}

// --- Capacity -------------------------------------------------------------

void test_alloc_exhaustion_returns_invalid() {
    Scheduler s;
    Counter c;
    s.poll(0);
    for (size_t i = 0; i < kMaxTimers; i++) {
        TEST_ASSERT_NOT_EQUAL(kInvalidTimer, s.after(1000, bump, &c));
    }
    TEST_ASSERT_EQUAL_INT(kInvalidTimer, s.after(1000, bump, &c));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_oneshot_fires_once_at_due);
    RUN_TEST(test_oneshot_fires_when_polled_late);
    RUN_TEST(test_periodic_fires_each_period);
    RUN_TEST(test_periodic_resyncs_after_long_stall_no_burst);
    RUN_TEST(test_multiple_timers_fire_in_one_poll);
    RUN_TEST(test_cancel_prevents_firing);
    RUN_TEST(test_cancel_from_callback);
    RUN_TEST(test_cancel_from_callback_lower_slot_first);
    RUN_TEST(test_rearm_from_callback);
    RUN_TEST(test_poll_returns_time_to_next);
    RUN_TEST(test_poll_returns_max_when_idle);
    RUN_TEST(test_deadline_survives_millis_wraparound);
    RUN_TEST(test_alloc_exhaustion_returns_invalid);
    return UNITY_END();
}
