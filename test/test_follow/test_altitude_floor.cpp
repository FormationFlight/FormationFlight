// Altitude floor clamping, the relative-altitude term, and targetTooFar()
// suppression. All driven through service() via the FollowHarness fakes.

#include <unity.h>

#include "test_helpers.h"

// Peer/self position shared by these tests -- arbitrary, chosen only so
// self and peer start co-located (target stays well inside maxTargetDistM
// unless a test deliberately moves self away).
static const double PEER_LAT = 37.0;
static const double PEER_LON = -122.0;

void test_altitude_above_floor_is_not_clamped() {
    FollowHarness h;
    h.fc.gcsNav = true;
    h.fc.altitudeCm = 1000;  // 10m home-relative
    h.setupLockedPeer(1, PEER_LAT, PEER_LON);

    h.tick();

    // altCm = localAltitudeCm(1000) + relalt(0)*100 + ofsVertM(10)*100
    //       = 1000 + 0 + 1000 = 2000, well above floorCm (minAltM=3.0 -> 300).
    TEST_ASSERT_EQUAL(1, (int)h.fc.sentWaypoints.size());
    TEST_ASSERT_EQUAL_INT32(2000, h.fc.sentWaypoints[0].alt_cm);
}

void test_altitude_below_floor_is_clamped_but_still_emitted() {
    FollowHarness h;
    h.fc.gcsNav = true;
    h.fc.altitudeCm = -2000;  // deep below home, e.g. leader descending
    h.setupLockedPeer(1, PEER_LAT, PEER_LON);

    h.tick();

    // altCmD = -2000 + 0 + 1000 = -1000, below floorCm(300) -> clamps to
    // exactly floorCm, and the waypoint must still be sent (clamp, not
    // suppress -- spec's explicit "don't regress" invariant).
    TEST_ASSERT_EQUAL(1, (int)h.fc.sentWaypoints.size());
    TEST_ASSERT_EQUAL_INT32(300, h.fc.sentWaypoints[0].alt_cm);
}

void test_floor_clamp_not_attributable_to_rc_reports_floor_clamped_condition() {
    FollowHarness h;
    h.fc.gcsNav = true;
    h.fc.altitudeCm = -2000;  // same breach as above, no RC involved at all
    h.setupLockedPeer(1, PEER_LAT, PEER_LON);

    FollowConfig cfg = configOf(h);
    cfg.conditionFlagsGvarIndex = 1;  // enable so status() reports it
    const char* err = nullptr;
    TEST_ASSERT_TRUE(h.apply(cfg, &err));

    h.tick();

    FollowStatus s = h.status();
    TEST_ASSERT_TRUE(s.haveConditionFlagsGvarValue);
    TEST_ASSERT_EQUAL(FOLLOW_CONDITION_FLOOR_CLAMPED, s.conditionFlagsGvarValue);
}

void test_floor_clamp_attributable_to_rc_reports_rc_invalid_gap_condition() {
    FollowHarness h;
    h.fc.gcsNav = true;
    h.fc.altitudeCm = 200;  // 2m home-relative
    h.setupLockedPeer(1, PEER_LAT, PEER_LON);

    FollowConfig cfg = configOf(h);
    cfg.rcVertChannel = 5;
    cfg.conditionFlagsGvarIndex = 1;
    const char* err = nullptr;
    TEST_ASSERT_TRUE(h.apply(cfg, &err));
    // Full-down stick: maps to -gap = -|ofsVertM| = -10 (was +10, "above" ->
    // "10m below") -- the other axes (long=-15) give 15m of horizontal
    // separation, well over minSepM(8), so the sign flip clears the RC
    // safety net and this candidate is actually adopted.
    h.fc.rc[5] = 1000;

    h.tick();

    // altCmD = 200 + 0 + (-10)*100 = -800, below floorCm(300) -> clamped.
    // Had the static default (+10) been used instead, altCmStatic = 200 +
    // 0 + 1000 = 1200 >= 300 -- no clamp -- so this breach is attributable
    // to RC, not the configured slot.
    TEST_ASSERT_EQUAL(1, (int)h.fc.sentWaypoints.size());
    TEST_ASSERT_EQUAL_INT32(300, h.fc.sentWaypoints[0].alt_cm);

    FollowStatus s = h.status();
    TEST_ASSERT_TRUE(s.haveConditionFlagsGvarValue);
    TEST_ASSERT_EQUAL(FOLLOW_CONDITION_RC_INVALID_GAP_SETTINGS, s.conditionFlagsGvarValue);
}

// ---- relative altitude ----

void test_relative_altitude_uses_peer_minus_self_msl() {
    FollowHarness h;
    h.fc.gcsNav = true;
    h.fc.altitudeCm = 1000;  // 10m home-relative
    // Leader 20m above us in MSL terms: peer alt 120m, self alt 100m at the
    // same lat/lon. v1 carried this as peer->relalt; v2 derives it from the
    // two MSL altitudes.
    h.setPeer(1, PEER_LAT, PEER_LON, /*speedMs=*/10.0, /*courseDeg=*/0.0, /*alt_m=*/120);
    h.self.set(PEER_LAT, PEER_LON, /*alt_m=*/100);

    h.tick();

    // altCm = localAltitudeCm(1000) + (120-100)*100 + ofsVertM(10)*100
    //       = 1000 + 2000 + 1000 = 4000.
    TEST_ASSERT_EQUAL(1, (int)h.fc.sentWaypoints.size());
    TEST_ASSERT_EQUAL_INT32(4000, h.fc.sentWaypoints[0].alt_cm);
}

// ---- §4.12 targetTooFar() ----

void test_target_within_max_dist_is_emitted_normally() {
    FollowHarness h;
    h.fc.gcsNav = true;
    h.setupLockedPeer(1, PEER_LAT, PEER_LON);  // self co-located with peer

    h.tick();

    TEST_ASSERT_EQUAL(1, (int)h.fc.sentWaypoints.size());
}

void test_target_beyond_max_dist_suppresses_waypoint_but_keeps_lock() {
    FollowHarness h;
    h.fc.gcsNav = true;

    // Peer at the origin; self ~1 degree of latitude away (~111km), far
    // beyond maxTargetDistM's default 50m.
    h.setPeer(1, 0.0, 0.0, /*speedMs=*/10.0, /*courseDeg=*/0.0);
    h.self.set(1.0, 0.0);

    FollowConfig cfg = configOf(h);
    cfg.conditionFlagsGvarIndex = 1;
    const char* err = nullptr;
    TEST_ASSERT_TRUE(h.apply(cfg, &err));

    h.tick();

    TEST_ASSERT_EQUAL(0, (int)h.fc.sentWaypoints.size());

    FollowStatus s = h.status();
    TEST_ASSERT_EQUAL(FOLLOW_LOCK_LOCKED, s.state);  // lock untouched
    TEST_ASSERT_EQUAL_UINT32(1, s.lockedUid);
    TEST_ASSERT_TRUE(s.haveConditionFlagsGvarValue);
    TEST_ASSERT_EQUAL(FOLLOW_CONDITION_TARGET_TOO_FAR, s.conditionFlagsGvarValue);
}
