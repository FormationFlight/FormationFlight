// slotToLatLon()'s bearing/distance derivation. Pure, no fakes.
//
// Test functions are plain (non-static) so test_main.cpp (this suite's one
// Unity entry point) can RUN_TEST() them by extern declaration -- see that
// file's comment for why every test_*.cpp in this directory follows this
// split-file-single-binary pattern.

#include <unity.h>
#include <cmath>

#include "FollowManager.h"
#include "../../src/lib/GNSS/GNSSManager.h"

// slotToLatLon() projects from a peer position; the tests below all use
// (0,0) as that origin so "north"/"east" map directly onto lat/lon sign.
static const int32_t ORIGIN_LAT_1E6 = 0;
static const int32_t ORIGIN_LON_1E6 = 0;

// ---- Four cardinal single-axis offsets at course_deg = 0 (due-north leader) ----

void test_ahead_at_course_zero_is_due_north()
{
    FollowTarget t = slotToLatLon(ORIGIN_LAT_1E6, ORIGIN_LON_1E6, 0.0, /*long_m=*/10.0, /*lat_m=*/0.0);
    TEST_ASSERT_TRUE(t.lat_1e7 > 0);   // north of origin
    TEST_ASSERT_INT32_WITHIN(50, 0, t.lon_1e7); // same meridian
}

void test_behind_at_course_zero_is_due_south()
{
    FollowTarget t = slotToLatLon(ORIGIN_LAT_1E6, ORIGIN_LON_1E6, 0.0, /*long_m=*/-10.0, /*lat_m=*/0.0);
    TEST_ASSERT_TRUE(t.lat_1e7 < 0);
    TEST_ASSERT_INT32_WITHIN(50, 0, t.lon_1e7);
}

void test_right_at_course_zero_is_due_east()
{
    FollowTarget t = slotToLatLon(ORIGIN_LAT_1E6, ORIGIN_LON_1E6, 0.0, /*long_m=*/0.0, /*lat_m=*/10.0);
    TEST_ASSERT_INT32_WITHIN(50, 0, t.lat_1e7);
    TEST_ASSERT_TRUE(t.lon_1e7 > 0);
}

void test_left_at_course_zero_is_due_west()
{
    FollowTarget t = slotToLatLon(ORIGIN_LAT_1E6, ORIGIN_LON_1E6, 0.0, /*long_m=*/0.0, /*lat_m=*/-10.0);
    TEST_ASSERT_INT32_WITHIN(50, 0, t.lat_1e7);
    TEST_ASSERT_TRUE(t.lon_1e7 < 0);
}

// ---- Combined offset (Behind 15m, the lat/lon-relevant half of the
// "chase-high" default -- vertical isn't part of this math) rotated by a
// non-zero course, verifying the rotation itself rather than just the
// axis-aligned cases above. ----

void test_behind_at_course_90_is_due_west()
{
    // Leader heading east (course 90): "behind" trails to the west.
    FollowTarget t = slotToLatLon(ORIGIN_LAT_1E6, ORIGIN_LON_1E6, 90.0, /*long_m=*/-15.0, /*lat_m=*/0.0);
    TEST_ASSERT_INT32_WITHIN(50, 0, t.lat_1e7);
    TEST_ASSERT_TRUE(t.lon_1e7 < 0);
}

void test_behind_at_course_270_is_due_east()
{
    // Leader heading west (course 270): "behind" trails to the east --
    // mirror image of the course-90 case, confirming the rotation's sign.
    FollowTarget t = slotToLatLon(ORIGIN_LAT_1E6, ORIGIN_LON_1E6, 270.0, /*long_m=*/-15.0, /*lat_m=*/0.0);
    TEST_ASSERT_INT32_WITHIN(50, 0, t.lat_1e7);
    TEST_ASSERT_TRUE(t.lon_1e7 > 0);
}

// ---- course_deg wraparound near the 0/360 boundary ----

void test_ahead_bearing_wraps_near_360()
{
    // Ahead always points along the course direction -- at course=350 the
    // derived bearing should land at ~350, not wrap incorrectly to
    // something near 0/negative. Cross-check against calculatePointAtDistance()
    // called directly with distance=long_m, bearing=350.
    FollowTarget t = slotToLatLon(ORIGIN_LAT_1E6, ORIGIN_LON_1E6, 350.0, /*long_m=*/10.0, /*lat_m=*/0.0);

    GNSSLocation origin{};
    GNSSLocation expected = GNSSManager::calculatePointAtDistance(origin, 10.0, 350.0);
    TEST_ASSERT_INT32_WITHIN(5, (int32_t)lround(expected.lat * 1e7), t.lat_1e7);
    TEST_ASSERT_INT32_WITHIN(5, (int32_t)lround(expected.lon * 1e7), t.lon_1e7);
}

// ---- Round-trip against GNSSManager::calculatePointAtDistance()'s own
// contract: independently derive the expected bearing/distance from
// (long_m, lat_m, course_deg) via plain vector rotation (not by re-deriving
// north_m/east_m the way slotToLatLon() itself does), and confirm feeding
// that bearing/distance through calculatePointAtDistance() lands on the
// exact same point slotToLatLon() produced. This checks slotToLatLon()'s
// derivation, not calculatePointAtDistance() itself. ----

static void assertRoundTrips(double course_deg, double long_m, double lat_m)
{
    FollowTarget actual = slotToLatLon(ORIGIN_LAT_1E6, ORIGIN_LON_1E6, course_deg, long_m, lat_m);

    // Independent derivation: rotating the (long_m, lat_m) vector by
    // course_deg is equivalent to a bearing of course_deg + angle-of(lat_m,
    // long_m) at magnitude hypot(long_m, lat_m) from the origin.
    double distance_m = std::sqrt(long_m * long_m + lat_m * lat_m);
    double bearing_deg = course_deg + degrees(std::atan2(lat_m, long_m));
    bearing_deg = std::fmod(bearing_deg, 360.0);
    if (bearing_deg < 0.0) bearing_deg += 360.0;

    GNSSLocation origin{};
    GNSSLocation expected = GNSSManager::calculatePointAtDistance(origin, distance_m, bearing_deg);

    TEST_ASSERT_INT32_WITHIN(5, (int32_t)lround(expected.lat * 1e7), actual.lat_1e7);
    TEST_ASSERT_INT32_WITHIN(5, (int32_t)lround(expected.lon * 1e7), actual.lon_1e7);
}

void test_round_trip_ahead()
{
    assertRoundTrips(0.0, 10.0, 0.0);
}

void test_round_trip_combined_offset_nontrivial_course()
{
    // chase-high's Behind 15m combined with a lateral component, at a
    // non-cardinal course -- exercises the general rotation case.
    assertRoundTrips(37.0, -15.0, 5.0);
}
