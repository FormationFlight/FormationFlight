// Heading modes (resolveHeadingDeg()) and course fallback
// (resolveCourseDeg()). Both private methods, driven through service().

#include <unity.h>

#include "test_helpers.h"

static const double SELF_LAT = 37.0;
static const double SELF_LON = -122.0;

static void setHeadingMode(FollowHarness& h, FollowHeadingMode mode, double headingDeg = 0.0) {
    FollowConfig cfg = configOf(h);
    cfg.headingMode = mode;
    cfg.headingDeg = headingDeg;
    const char* err = nullptr;
    TEST_ASSERT_TRUE(h.apply(cfg, &err));
}

// ---- §4.5 heading modes ----

void test_heading_off_never_sends_set_head_even_with_heading_hold_active() {
    FollowHarness h;
    h.fc.gcsNav = true;
    h.fc.headingHold = true;
    h.setupLockedPeer(1, SELF_LAT, SELF_LON, /*speedMs=*/10.0, /*courseDeg=*/45.0);
    setHeadingMode(h, FOLLOW_HEADING_OFF);

    h.tick();

    TEST_ASSERT_EQUAL(1, (int)h.fc.sentWaypoints.size());
    TEST_ASSERT_EQUAL_INT16(0, h.fc.sentWaypoints[0].headingDeg);  // wire sentinel
    TEST_ASSERT_EQUAL(0, (int)h.fc.sentHeadings.size());
}

void test_heading_course_returns_course_deg_wrapped() {
    FollowHarness h;
    h.fc.gcsNav = true;
    h.fc.headingHold = true;
    h.setupLockedPeer(1, SELF_LAT, SELF_LON, /*speedMs=*/10.0, /*courseDeg=*/45.0);
    setHeadingMode(h, FOLLOW_HEADING_COURSE);

    h.tick();

    TEST_ASSERT_EQUAL(1, (int)h.fc.sentWaypoints.size());
    TEST_ASSERT_EQUAL_INT16(45, h.fc.sentWaypoints[0].headingDeg);
    TEST_ASSERT_EQUAL(1, (int)h.fc.sentHeadings.size());
    TEST_ASSERT_EQUAL_INT16(45, h.fc.sentHeadings[0]);
}

void test_heading_fixed_ignores_course_deg() {
    FollowHarness h;
    h.fc.gcsNav = true;
    h.setupLockedPeer(1, SELF_LAT, SELF_LON, /*speedMs=*/10.0, /*courseDeg=*/45.0);
    setHeadingMode(h, FOLLOW_HEADING_FIXED, /*headingDeg=*/200.0);

    h.tick();

    TEST_ASSERT_EQUAL(1, (int)h.fc.sentWaypoints.size());
    TEST_ASSERT_EQUAL_INT16(200, h.fc.sentWaypoints[0].headingDeg);
}

void test_heading_course_relative_adds_offset_and_wraps() {
    FollowHarness h;
    h.fc.gcsNav = true;
    h.setupLockedPeer(1, SELF_LAT, SELF_LON, /*speedMs=*/10.0, /*courseDeg=*/45.0);
    setHeadingMode(h, FOLLOW_HEADING_COURSE_RELATIVE, /*headingDeg=*/350.0);

    h.tick();

    // 45 + 350 = 395 -> wraps to 35.
    TEST_ASSERT_EQUAL(1, (int)h.fc.sentWaypoints.size());
    TEST_ASSERT_EQUAL_INT16(35, h.fc.sentWaypoints[0].headingDeg);
}

void test_heading_point_leader_bears_toward_peer_position() {
    FollowHarness h;
    h.fc.gcsNav = true;

    // Peer 30m from self at bearing 45 -- close enough to stay well inside
    // maxTargetDistM's default 50m once the (small) chase offset is applied.
    double peerLat = 0.0;
    double peerLon = 0.0;
    geo::pointAtDistance(SELF_LAT, SELF_LON, 30.0, 45.0, peerLat, peerLon);

    h.setPeer(1, peerLat, peerLon, /*speedMs=*/10.0, /*courseDeg=*/200.0);
    h.self.set(SELF_LAT, SELF_LON);
    setHeadingMode(h, FOLLOW_HEADING_POINT_LEADER);

    h.tick();

    TEST_ASSERT_EQUAL(1, (int)h.fc.sentWaypoints.size());
    TEST_ASSERT_INT16_WITHIN(2, 45, h.fc.sentWaypoints[0].headingDeg);
}

void test_heading_zero_collision_remaps_to_one() {
    FollowHarness h;
    h.fc.gcsNav = true;
    h.fc.headingHold = true;
    // Leader heading exactly due north (course 0) -- COURSE mode would
    // otherwise resolve to the literal wire value 0, colliding with the
    // "don't update heading" sentinel.
    h.setupLockedPeer(1, SELF_LAT, SELF_LON, /*speedMs=*/10.0, /*courseDeg=*/0.0);
    setHeadingMode(h, FOLLOW_HEADING_COURSE);

    h.tick();

    TEST_ASSERT_EQUAL(1, (int)h.fc.sentWaypoints.size());
    TEST_ASSERT_EQUAL_INT16(1, h.fc.sentWaypoints[0].headingDeg);
    // And it must still count as "send a heading" at the service() gate, not
    // get treated as the OFF sentinel.
    TEST_ASSERT_EQUAL(1, (int)h.fc.sentHeadings.size());
    TEST_ASSERT_EQUAL_INT16(1, h.fc.sentHeadings[0]);
}

void test_nonzero_heading_skips_send_set_head_when_heading_hold_inactive() {
    FollowHarness h;
    h.fc.gcsNav = true;
    h.fc.headingHold = false;  // INAV's HEADING HOLD box not active
    h.setupLockedPeer(1, SELF_LAT, SELF_LON, /*speedMs=*/10.0, /*courseDeg=*/45.0);
    setHeadingMode(h, FOLLOW_HEADING_COURSE);

    h.tick();

    // WP#255's p1 write is unconditional...
    TEST_ASSERT_EQUAL(1, (int)h.fc.sentWaypoints.size());
    TEST_ASSERT_EQUAL_INT16(45, h.fc.sentWaypoints[0].headingDeg);
    // ...but MSP_SET_HEAD is gated on headingHoldActive().
    TEST_ASSERT_EQUAL(0, (int)h.fc.sentHeadings.size());
}

// ---- §4.6 course fallback ----

void test_course_above_threshold_uses_live_ground_course() {
    FollowHarness h;
    h.fc.gcsNav = true;
    h.setupLockedPeer(1, SELF_LAT, SELF_LON, /*speedMs=*/10.0, /*courseDeg=*/45.0);
    setHeadingMode(h, FOLLOW_HEADING_COURSE);

    h.tick();
    TEST_ASSERT_EQUAL(1, (int)h.fc.sentWaypoints.size());
    TEST_ASSERT_EQUAL_INT16(45, h.fc.sentWaypoints[0].headingDeg);

    // A second cycle with a different live course tracks the new value --
    // proves this isn't accidentally latched.
    h.setPeer(1, SELF_LAT, SELF_LON, /*speedMs=*/10.0, /*courseDeg=*/99.0);
    h.tick();
    TEST_ASSERT_EQUAL(2, (int)h.fc.sentWaypoints.size());
    TEST_ASSERT_EQUAL_INT16(99, h.fc.sentWaypoints[1].headingDeg);
}

void test_course_dropping_below_threshold_holds_last_valid_course() {
    FollowHarness h;
    h.fc.gcsNav = true;
    h.setupLockedPeer(1, SELF_LAT, SELF_LON, /*speedMs=*/10.0, /*courseDeg=*/45.0);
    setHeadingMode(h, FOLLOW_HEADING_COURSE);

    h.tick();  // captures a valid course (45) at speed above minCourseSpeed(2 m/s)
    TEST_ASSERT_EQUAL(1, (int)h.fc.sentWaypoints.size());
    TEST_ASSERT_EQUAL_INT16(45, h.fc.sentWaypoints[0].headingDeg);

    // Leader slows to a near-stop; ground course reading gets jittery (200)
    // -- must hold 45, not adopt 200.
    h.setPeer(1, SELF_LAT, SELF_LON, /*speedMs=*/0.5, /*courseDeg=*/200.0);
    h.tick();
    TEST_ASSERT_EQUAL(2, (int)h.fc.sentWaypoints.size());
    TEST_ASSERT_EQUAL_INT16(45, h.fc.sentWaypoints[1].headingDeg);
}

void test_course_below_threshold_from_first_cycle_falls_back_to_reported_value() {
    FollowHarness h;
    h.fc.gcsNav = true;
    // Never above minCourseSpeed -- no valid course ever captured, so this
    // must fall back to whatever's reported (77) rather than an arbitrary 0.
    h.setupLockedPeer(1, SELF_LAT, SELF_LON, /*speedMs=*/0.5, /*courseDeg=*/77.0);
    setHeadingMode(h, FOLLOW_HEADING_COURSE);

    h.tick();
    TEST_ASSERT_EQUAL(1, (int)h.fc.sentWaypoints.size());
    TEST_ASSERT_EQUAL_INT16(77, h.fc.sentWaypoints[0].headingDeg);
}
