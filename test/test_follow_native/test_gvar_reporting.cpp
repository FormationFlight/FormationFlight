// Status/condition GVAR reporting: updateStatusGvars()/
// updateAutothrottleGvars()'s change+heartbeat send rule and
// raiseCondition()'s priority ordering.

#include <unity.h>

#include "test_helpers.h"

static int countGvarSends(FakeMsp &msp, uint8_t index)
{
    int n = 0;
    for (auto &g : msp.sentGvars) if (g.index == index) n++;
    return n;
}

void test_gvar_first_cycle_always_sends_even_when_value_is_zero()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers; // no peer -> state stays IDLE, statusGvarValue == 0
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = false; // gate inactive keeps state IDLE

    FollowRuntimeConfig cfg = fm.getConfig();
    cfg.statusGvarIndex = 1;
    String err;
    TEST_ASSERT_TRUE(fm.applyConfig(cfg, &err));

    followTick(fm);
    TEST_ASSERT_EQUAL(1, countGvarSends(msp, 1));
    TEST_ASSERT_EQUAL(0, msp.sentGvars.back().value); // explicit 0, not skipped
}

void test_gvar_resends_immediately_when_value_changes()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true; // gate active, but no peer yet -> ACQUIRING (value 1)

    FollowRuntimeConfig cfg = fm.getConfig();
    cfg.statusGvarIndex = 1;
    String err;
    TEST_ASSERT_TRUE(fm.applyConfig(cfg, &err));

    followTick(fm);
    TEST_ASSERT_EQUAL(1, countGvarSends(msp, 1));
    TEST_ASSERT_EQUAL(1, msp.sentGvars.back().value); // ACQUIRING

    // Add a peer -> LOCKED (value 2) on the very next cycle, well under the
    // 5s heartbeat -- must resend immediately because the value changed.
    peers.setPeer(0, /*id=*/1, 37.0, -122.0, 10.0, 0.0);
    GNSSLocation self{};
    self.lat = 37.0;
    self.lon = -122.0;
    gnss.setSelf(self);

    followTick(fm);
    TEST_ASSERT_EQUAL(2, countGvarSends(msp, 1));
    TEST_ASSERT_EQUAL(2, msp.sentGvars.back().value); // LOCKED
}

void test_gvar_unchanged_value_not_resent_before_heartbeat()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = false; // gate inactive -> IDLE -> value 0, stable across cycles

    FollowRuntimeConfig cfg = fm.getConfig();
    cfg.statusGvarIndex = 1;
    String err;
    TEST_ASSERT_TRUE(fm.applyConfig(cfg, &err));

    followTick(fm); // first cycle: sentinel -> always sends
    TEST_ASSERT_EQUAL(1, countGvarSends(msp, 1));

    followTick(fm); // unchanged value, well under 5000ms heartbeat -> no resend
    TEST_ASSERT_EQUAL(1, countGvarSends(msp, 1));
}

void test_gvar_unchanged_value_resent_after_heartbeat_elapses()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = false; // gate inactive -> IDLE -> value 0, stable across cycles

    FollowRuntimeConfig cfg = fm.getConfig();
    cfg.statusGvarIndex = 1;
    String err;
    TEST_ASSERT_TRUE(fm.applyConfig(cfg, &err));

    followTick(fm);
    TEST_ASSERT_EQUAL(1, countGvarSends(msp, 1));

    native_millis_advance(5100); // past FOLLOW_GVAR_HEARTBEAT_MS (5000)
    fm.loop();
    TEST_ASSERT_EQUAL(2, countGvarSends(msp, 1));
    TEST_ASSERT_EQUAL(0, msp.sentGvars.back().value); // same value, resent as heartbeat
}

void test_gvar_index_minus_one_never_sends()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;
    // All GVAR indices stay at their -1 default.

    followTick(fm); // ACQUIRING
    peers.setPeer(0, /*id=*/1, 37.0, -122.0, 10.0, 0.0);
    GNSSLocation self{};
    self.lat = 37.0;
    self.lon = -122.0;
    gnss.setSelf(self);
    followTick(fm); // LOCKED, waypoint emitted

    TEST_ASSERT_EQUAL(0, (int)msp.sentGvars.size());
}

void test_condition_code_priority_highest_value_wins_not_first_computed()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;
    msp.altitudeCm = 1000; // keep the floor check out of the way

    FollowRuntimeConfig cfg = fm.getConfig();
    cfg.ofsLongM = -15.0;
    cfg.ofsLatM = 0.0;
    cfg.ofsVertM = 0.0; // shrinks the other-axes magnitude so the RC flip below gets rejected
    cfg.rcLongChannel = 5;
    cfg.conditionFlagsGvarIndex = 1;
    String err;
    TEST_ASSERT_TRUE(fm.applyConfig(cfg, &err));

    // Peer far from self -> targetTooFar() will independently want to raise
    // TARGET_TOO_FAR (2), computed *after* the RC-frozen condition below.
    peers.setPeer(0, /*id=*/1, 0.0, 0.0, 10.0, 0.0);
    GNSSLocation self{};
    self.lat = 1.0; // ~111km away, way past maxTargetDistM
    self.lon = 0.0;
    gnss.setSelf(self);

    // Full-up stick: candidate.long flips sign vs lastKnownGood(-15) while
    // the other axes (lat=0, vert=0) give zero separation -> rejected by
    // the RC safety net -> rcSlotFrozen -> raises RC_INVALID_GAP_SETTINGS (3),
    // computed *before* TARGET_TOO_FAR (2) in loop()'s call order.
    msp.rcChannelUs[5] = 2000;

    followTick(fm);

    DynamicJsonDocument doc(1024);
    fm.statusJson(&doc);
    // 3 (higher) must win even though 2 was computed later.
    TEST_ASSERT_EQUAL(FOLLOW_CONDITION_RC_INVALID_GAP_SETTINGS, doc["conditionFlagsGvarValue"].as<int>());
}
