// Status/condition GVAR reporting: updateStatusGvars()/
// updateAutothrottleGvars()'s change+heartbeat send rule and
// raiseCondition()'s priority ordering.

#include <unity.h>

#include "test_helpers.h"

static int countGvarSends(const FakeFc& fc, uint8_t index) {
    int n = 0;
    for (const auto& g : fc.sentGvars) {
        if (g.index == index) n++;
    }
    return n;
}

void test_gvar_first_cycle_always_sends_even_when_value_is_zero() {
    FollowHarness h;         // no peer -> state stays IDLE, statusGvarValue == 0
    h.fc.gcsNav = false;     // gate inactive keeps state IDLE

    FollowConfig cfg = configOf(h);
    cfg.statusGvarIndex = 1;
    const char* err = nullptr;
    TEST_ASSERT_TRUE(h.apply(cfg, &err));

    h.tick();
    TEST_ASSERT_EQUAL(1, countGvarSends(h.fc, 1));
    TEST_ASSERT_EQUAL(0, h.fc.sentGvars.back().value);  // explicit 0, not skipped
}

void test_gvar_resends_immediately_when_value_changes() {
    FollowHarness h;
    h.fc.gcsNav = true;  // gate active, but no peer yet -> ACQUIRING (value 1)

    FollowConfig cfg = configOf(h);
    cfg.statusGvarIndex = 1;
    const char* err = nullptr;
    TEST_ASSERT_TRUE(h.apply(cfg, &err));

    h.tick();
    TEST_ASSERT_EQUAL(1, countGvarSends(h.fc, 1));
    TEST_ASSERT_EQUAL(1, h.fc.sentGvars.back().value);  // ACQUIRING

    // Add a peer -> LOCKED (value 2) on the very next cycle, well under the
    // 5s heartbeat -- must resend immediately because the value changed.
    h.setPeer(/*uid=*/1, 37.0, -122.0, 10.0, 0.0);
    h.self.set(37.0, -122.0);

    h.tick();
    TEST_ASSERT_EQUAL(2, countGvarSends(h.fc, 1));
    TEST_ASSERT_EQUAL(2, h.fc.sentGvars.back().value);  // LOCKED
}

void test_gvar_unchanged_value_not_resent_before_heartbeat() {
    FollowHarness h;
    h.fc.gcsNav = false;  // gate inactive -> IDLE -> value 0, stable across cycles

    FollowConfig cfg = configOf(h);
    cfg.statusGvarIndex = 1;
    const char* err = nullptr;
    TEST_ASSERT_TRUE(h.apply(cfg, &err));

    h.tick();  // first cycle: sentinel -> always sends
    TEST_ASSERT_EQUAL(1, countGvarSends(h.fc, 1));

    h.tick();  // unchanged value, well under the 5000ms heartbeat -> no resend
    TEST_ASSERT_EQUAL(1, countGvarSends(h.fc, 1));
}

void test_gvar_unchanged_value_resent_after_heartbeat_elapses() {
    FollowHarness h;
    h.fc.gcsNav = false;  // gate inactive -> IDLE -> value 0, stable across cycles

    FollowConfig cfg = configOf(h);
    cfg.statusGvarIndex = 1;
    const char* err = nullptr;
    TEST_ASSERT_TRUE(h.apply(cfg, &err));

    h.tick();
    TEST_ASSERT_EQUAL(1, countGvarSends(h.fc, 1));

    h.now += kFollowGvarHeartbeatMs + 100;  // past the heartbeat (5000ms)
    h.ctl.service(h.now);
    TEST_ASSERT_EQUAL(2, countGvarSends(h.fc, 1));
    TEST_ASSERT_EQUAL(0, h.fc.sentGvars.back().value);  // same value, resent as heartbeat
}

void test_gvar_index_minus_one_never_sends() {
    FollowHarness h;
    h.fc.gcsNav = true;
    // All GVAR indices stay at their -1 default.

    h.tick();  // ACQUIRING
    h.setPeer(/*uid=*/1, 37.0, -122.0, 10.0, 0.0);
    h.self.set(37.0, -122.0);
    h.tick();  // LOCKED, waypoint emitted

    TEST_ASSERT_EQUAL(1, static_cast<int>(h.fc.sentWaypoints.size()));
    TEST_ASSERT_EQUAL(0, static_cast<int>(h.fc.sentGvars.size()));
}

void test_condition_code_priority_highest_value_wins_not_first_computed() {
    FollowHarness h;
    h.fc.gcsNav = true;
    h.fc.altitudeCm = 1000;  // keep the floor check out of the way

    FollowConfig cfg = configOf(h);
    cfg.ofsLongM = -15.0;
    cfg.ofsLatM = 0.0;
    cfg.ofsVertM = 0.0;  // shrinks the other-axes magnitude so the RC flip below gets rejected
    cfg.rcLongChannel = 5;
    cfg.conditionFlagsGvarIndex = 1;
    const char* err = nullptr;
    TEST_ASSERT_TRUE(h.apply(cfg, &err));

    // Peer far from self -> targetTooFar() will independently want to raise
    // TARGET_TOO_FAR (2), computed *after* the RC-frozen condition below.
    h.setPeer(/*uid=*/1, 0.0, 0.0, 10.0, 0.0);
    h.self.set(1.0, 0.0);  // ~111km away, way past maxTargetDistM

    // Full-up stick: candidate.long flips sign vs lastKnownGood(-15) while
    // the other axes (lat=0, vert=0) give zero separation -> rejected by
    // the RC safety net -> rcSlotFrozen -> raises RC_INVALID_GAP_SETTINGS (3),
    // computed *before* TARGET_TOO_FAR (2) in service()'s call order.
    h.fc.rc[5] = 2000;

    h.tick();

    FollowStatus s = h.status();
    // 3 (higher) must win even though 2 was computed later.
    TEST_ASSERT_TRUE(s.haveConditionFlagsGvarValue);
    TEST_ASSERT_EQUAL(FOLLOW_CONDITION_RC_INVALID_GAP_SETTINGS, s.conditionFlagsGvarValue);
}
