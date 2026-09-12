// Heading modes (resolveHeadingDeg()) and course fallback
// (resolveCourseDeg()). Both private methods, driven through loop().

#include <unity.h>

#include "test_helpers.h"
#include "../../src/lib/GNSS/GNSSManager.h"

static const double SELF_LAT = 37.0;
static const double SELF_LON = -122.0;

static void setHeadingMode(FollowManager &fm, FollowHeadingMode mode, double headingDeg = 0.0)
{
    FollowRuntimeConfig cfg = fm.getConfig();
    cfg.headingMode = mode;
    cfg.headingDeg = headingDeg;
    String err;
    TEST_ASSERT_TRUE(fm.applyConfig(cfg, &err));
}

// ---- §4.5 heading modes ----

void test_heading_off_never_sends_set_head_even_with_heading_hold_active()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;
    msp.headingHoldActive = true;
    setupLockedPeer(peers, gnss, SELF_LAT, SELF_LON, /*speedMs=*/10.0, /*courseDeg=*/45.0);
    setHeadingMode(fm, FOLLOW_HEADING_OFF);

    followTick(fm);

    TEST_ASSERT_EQUAL(1, (int)msp.sentWaypoints.size());
    TEST_ASSERT_EQUAL_INT16(0, msp.sentWaypoints[0].headingDeg); // wire sentinel
    TEST_ASSERT_EQUAL(0, (int)msp.sentHeadings.size());
}

void test_heading_course_returns_course_deg_wrapped()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;
    msp.headingHoldActive = true;
    setupLockedPeer(peers, gnss, SELF_LAT, SELF_LON, /*speedMs=*/10.0, /*courseDeg=*/45.0);
    setHeadingMode(fm, FOLLOW_HEADING_COURSE);

    followTick(fm);

    TEST_ASSERT_EQUAL_INT16(45, msp.sentWaypoints[0].headingDeg);
    TEST_ASSERT_EQUAL(1, (int)msp.sentHeadings.size());
    TEST_ASSERT_EQUAL_INT16(45, msp.sentHeadings[0]);
}

void test_heading_fixed_ignores_course_deg()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;
    setupLockedPeer(peers, gnss, SELF_LAT, SELF_LON, /*speedMs=*/10.0, /*courseDeg=*/45.0);
    setHeadingMode(fm, FOLLOW_HEADING_FIXED, /*headingDeg=*/200.0);

    followTick(fm);

    TEST_ASSERT_EQUAL_INT16(200, msp.sentWaypoints[0].headingDeg);
}

void test_heading_course_relative_adds_offset_and_wraps()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;
    setupLockedPeer(peers, gnss, SELF_LAT, SELF_LON, /*speedMs=*/10.0, /*courseDeg=*/45.0);
    setHeadingMode(fm, FOLLOW_HEADING_COURSE_RELATIVE, /*headingDeg=*/350.0);

    followTick(fm);

    // 45 + 350 = 395 -> wraps to 35.
    TEST_ASSERT_EQUAL_INT16(35, msp.sentWaypoints[0].headingDeg);
}

void test_heading_point_leader_bears_toward_peer_position()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;

    GNSSLocation self{};
    self.lat = SELF_LAT;
    self.lon = SELF_LON;
    // Peer 30m from self at bearing 45 -- close enough to stay well inside
    // maxTargetDistM's default 50m once the (small) chase offset is applied.
    GNSSLocation peerLoc = GNSSManager::calculatePointAtDistance(self, 30.0, 45.0);

    peers.setPeer(0, /*id=*/1, peerLoc.lat, peerLoc.lon, /*speedMs=*/10.0, /*courseDeg=*/200.0);
    gnss.setSelf(self);
    setHeadingMode(fm, FOLLOW_HEADING_POINT_LEADER);

    followTick(fm);

    TEST_ASSERT_EQUAL(1, (int)msp.sentWaypoints.size());
    TEST_ASSERT_INT16_WITHIN(2, 45, msp.sentWaypoints[0].headingDeg);
}

void test_heading_zero_collision_remaps_to_one()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;
    msp.headingHoldActive = true;
    // Leader heading exactly due north (course 0) -- COURSE mode would
    // otherwise resolve to the literal wire value 0, colliding with the
    // "don't update heading" sentinel.
    setupLockedPeer(peers, gnss, SELF_LAT, SELF_LON, /*speedMs=*/10.0, /*courseDeg=*/0.0);
    setHeadingMode(fm, FOLLOW_HEADING_COURSE);

    followTick(fm);

    TEST_ASSERT_EQUAL_INT16(1, msp.sentWaypoints[0].headingDeg);
    // And it must still count as "send a heading" at the loop() gate, not
    // get treated as the OFF sentinel.
    TEST_ASSERT_EQUAL(1, (int)msp.sentHeadings.size());
    TEST_ASSERT_EQUAL_INT16(1, msp.sentHeadings[0]);
}

void test_nonzero_heading_skips_send_set_head_when_heading_hold_inactive()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;
    msp.headingHoldActive = false; // INAV's HEADING HOLD box not active
    setupLockedPeer(peers, gnss, SELF_LAT, SELF_LON, /*speedMs=*/10.0, /*courseDeg=*/45.0);
    setHeadingMode(fm, FOLLOW_HEADING_COURSE);

    followTick(fm);

    // WP#255's p1 write is unconditional...
    TEST_ASSERT_EQUAL_INT16(45, msp.sentWaypoints[0].headingDeg);
    // ...but MSP_SET_HEAD is gated on isHeadingHoldActive().
    TEST_ASSERT_EQUAL(0, (int)msp.sentHeadings.size());
}

// ---- §4.6 course fallback ----

void test_course_above_threshold_uses_live_ground_course()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;
    setupLockedPeer(peers, gnss, SELF_LAT, SELF_LON, /*speedMs=*/10.0, /*courseDeg=*/45.0);
    setHeadingMode(fm, FOLLOW_HEADING_COURSE);

    followTick(fm);
    TEST_ASSERT_EQUAL_INT16(45, msp.sentWaypoints[0].headingDeg);

    // A second cycle with a different live course tracks the new value --
    // proves this isn't accidentally latched.
    peers.setPeer(0, /*id=*/1, SELF_LAT, SELF_LON, /*speedMs=*/10.0, /*courseDeg=*/99.0);
    followTick(fm);
    TEST_ASSERT_EQUAL_INT16(99, msp.sentWaypoints[1].headingDeg);
}

void test_course_dropping_below_threshold_holds_last_valid_course()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;
    setupLockedPeer(peers, gnss, SELF_LAT, SELF_LON, /*speedMs=*/10.0, /*courseDeg=*/45.0);
    setHeadingMode(fm, FOLLOW_HEADING_COURSE);

    followTick(fm); // captures a valid course (45) at speed above minCourseSpeed(2 m/s)
    TEST_ASSERT_EQUAL_INT16(45, msp.sentWaypoints[0].headingDeg);

    // Leader slows to a near-stop; ground course reading gets jittery (200)
    // -- must hold 45, not adopt 200.
    peers.setPeer(0, /*id=*/1, SELF_LAT, SELF_LON, /*speedMs=*/0.5, /*courseDeg=*/200.0);
    followTick(fm);
    TEST_ASSERT_EQUAL_INT16(45, msp.sentWaypoints[1].headingDeg);
}

void test_course_below_threshold_from_first_cycle_falls_back_to_reported_value()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;
    // Never above minCourseSpeed -- no valid course ever captured, so this
    // must fall back to whatever's reported (77) rather than an arbitrary 0.
    setupLockedPeer(peers, gnss, SELF_LAT, SELF_LON, /*speedMs=*/0.5, /*courseDeg=*/77.0);
    setHeadingMode(fm, FOLLOW_HEADING_COURSE);

    followTick(fm);
    TEST_ASSERT_EQUAL_INT16(77, msp.sentWaypoints[0].headingDeg);
}
