// Altitude floor clamping and targetTooFar() suppression. Both driven
// through loop() via FakeMsp/FakeGnss/FakePeers.

#include <unity.h>
#include <cstring>

#include "test_helpers.h"

// Peer/self position shared by these tests -- arbitrary, chosen only so
// self and peer start co-located (target stays well inside maxTargetDistM
// unless a test deliberately moves self away).
static const double PEER_LAT = 37.0;
static const double PEER_LON = -122.0;

void test_altitude_above_floor_is_not_clamped()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;
    msp.altitudeCm = 1000; // 10m home-relative
    setupLockedPeer(peers, gnss, PEER_LAT, PEER_LON);

    followTick(fm);

    // altCm = local_altitude_cm(1000) + relalt(0)*100 + ofsVertM(10)*100
    //       = 1000 + 0 + 1000 = 2000, well above floorCm (minAltM=3.0 -> 300).
    TEST_ASSERT_EQUAL(1, (int)msp.sentWaypoints.size());
    TEST_ASSERT_EQUAL_INT32(2000, msp.sentWaypoints[0].alt_cm);
}

void test_altitude_below_floor_is_clamped_but_still_emitted()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;
    msp.altitudeCm = -2000; // deep below home, e.g. leader descending
    setupLockedPeer(peers, gnss, PEER_LAT, PEER_LON);

    followTick(fm);

    // altCmD = -2000 + 0 + 1000 = -1000, below floorCm(300) -> clamps to
    // exactly floorCm, and the waypoint must still be sent (clamp, not
    // suppress -- spec's explicit "don't regress" invariant).
    TEST_ASSERT_EQUAL(1, (int)msp.sentWaypoints.size());
    TEST_ASSERT_EQUAL_INT32(300, msp.sentWaypoints[0].alt_cm);
}

void test_floor_clamp_not_attributable_to_rc_reports_floor_clamped_condition()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;
    msp.altitudeCm = -2000; // same breach as above, no RC involved at all
    setupLockedPeer(peers, gnss, PEER_LAT, PEER_LON);

    FollowRuntimeConfig cfg = fm.getConfig();
    cfg.conditionFlagsGvarIndex = 1; // enable so statusJson() reports it
    String err;
    TEST_ASSERT_TRUE(fm.applyConfig(cfg, &err));

    followTick(fm);

    DynamicJsonDocument doc(1024);
    fm.statusJson(&doc);
    TEST_ASSERT_EQUAL(FOLLOW_CONDITION_FLOOR_CLAMPED, doc["conditionFlagsGvarValue"].as<int>());
}

void test_floor_clamp_attributable_to_rc_reports_rc_invalid_gap_condition()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;
    msp.altitudeCm = 200; // 2m home-relative
    setupLockedPeer(peers, gnss, PEER_LAT, PEER_LON);

    FollowRuntimeConfig cfg = fm.getConfig();
    cfg.rcVertChannel = 5;
    cfg.conditionFlagsGvarIndex = 1;
    String err;
    TEST_ASSERT_TRUE(fm.applyConfig(cfg, &err));
    // Full-down stick: maps to -gap = -|ofsVertM| = -10 (was +10, "above" ->
    // "10m below") -- the other axes (long=-15) give 15m of horizontal
    // separation, well over minSepM(8), so the sign flip clears the RC
    // safety net and this candidate is actually adopted.
    msp.rcChannelUs[5] = 1000;

    followTick(fm);

    // altCmD = 200 + 0 + (-10)*100 = -800, below floorCm(300) -> clamped.
    // Had the static default (+10) been used instead, altCmStatic = 200 +
    // 0 + 1000 = 1200 >= 300 -- no clamp -- so this breach is attributable
    // to RC, not the configured slot.
    TEST_ASSERT_EQUAL(1, (int)msp.sentWaypoints.size());
    TEST_ASSERT_EQUAL_INT32(300, msp.sentWaypoints[0].alt_cm);

    DynamicJsonDocument doc(1024);
    fm.statusJson(&doc);
    TEST_ASSERT_EQUAL(FOLLOW_CONDITION_RC_INVALID_GAP_SETTINGS, doc["conditionFlagsGvarValue"].as<int>());
}

// ---- §4.12 targetTooFar() ----

void test_target_within_max_dist_is_emitted_normally()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;
    setupLockedPeer(peers, gnss, PEER_LAT, PEER_LON); // self co-located with peer

    followTick(fm);

    TEST_ASSERT_EQUAL(1, (int)msp.sentWaypoints.size());
}

void test_target_beyond_max_dist_suppresses_waypoint_but_keeps_lock()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;

    // Peer at the origin; self ~1 degree of latitude away (~111km), far
    // beyond maxTargetDistM's default 50m.
    peers.setPeer(0, /*id=*/1, 0.0, 0.0, 10.0, 0.0);
    GNSSLocation self{};
    self.lat = 1.0;
    self.lon = 0.0;
    gnss.setSelf(self);

    FollowRuntimeConfig cfg = fm.getConfig();
    cfg.conditionFlagsGvarIndex = 1;
    String err;
    TEST_ASSERT_TRUE(fm.applyConfig(cfg, &err));

    followTick(fm);

    TEST_ASSERT_EQUAL(0, (int)msp.sentWaypoints.size());

    DynamicJsonDocument doc(1024);
    fm.statusJson(&doc);
    TEST_ASSERT_EQUAL_STRING("LOCKED", doc["state"].as<const char *>()); // lock untouched
    TEST_ASSERT_EQUAL(FOLLOW_CONDITION_TARGET_TOO_FAR, doc["conditionFlagsGvarValue"].as<int>());
}
