#include <unity.h>

#include <cmath>

#include "geo.h"

using namespace ff;

void setUp() {}
void tearDown() {}

// Reference values from the same spherical formulas (R = 6371 km), so these
// pin the port against v1's GNSSManager math rather than a different model.

void test_distance_zero_for_same_point() {
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, geo::distanceM(37.0, -122.0, 37.0, -122.0));
}

void test_distance_one_degree_of_latitude() {
    // One degree of arc on a 6371 km sphere = 111194.9 m.
    TEST_ASSERT_DOUBLE_WITHIN(0.5, 111194.93, geo::distanceM(0.0, 0.0, 1.0, 0.0));
}

void test_distance_is_symmetric() {
    const double a = geo::distanceM(37.0, -122.0, 37.001, -122.002);
    const double b = geo::distanceM(37.001, -122.002, 37.0, -122.0);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, a, b);
}

void test_bearing_cardinal_directions() {
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.0, geo::bearingDeg(0.0, 0.0, 1.0, 0.0));
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 90.0, geo::bearingDeg(0.0, 0.0, 0.0, 1.0));
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 180.0, geo::bearingDeg(0.0, 0.0, -1.0, 0.0));
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 270.0, geo::bearingDeg(0.0, 0.0, 0.0, -1.0));
}

void test_bearing_is_in_0_360() {
    for (int b = 0; b < 360; b += 15) {
        double lat, lon;
        geo::pointAtDistance(37.0, -122.0, 100.0, b, lat, lon);
        const double back = geo::bearingDeg(37.0, -122.0, lat, lon);
        TEST_ASSERT_TRUE(back >= 0.0 && back < 360.0);
        TEST_ASSERT_DOUBLE_WITHIN(0.01, static_cast<double>(b), back);
    }
}

void test_point_at_distance_round_trips_through_distance_and_bearing() {
    double lat, lon;
    geo::pointAtDistance(37.0, -122.0, 30.0, 45.0, lat, lon);
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 30.0, geo::distanceM(37.0, -122.0, lat, lon));
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 45.0, geo::bearingDeg(37.0, -122.0, lat, lon));
}

void test_point_at_distance_due_north_changes_only_latitude() {
    double lat, lon;
    geo::pointAtDistance(10.0, 20.0, 1000.0, 0.0, lat, lon);
    TEST_ASSERT_TRUE(lat > 10.0);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 20.0, lon);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_distance_zero_for_same_point);
    RUN_TEST(test_distance_one_degree_of_latitude);
    RUN_TEST(test_distance_is_symmetric);
    RUN_TEST(test_bearing_cardinal_directions);
    RUN_TEST(test_bearing_is_in_0_360);
    RUN_TEST(test_point_at_distance_round_trips_through_distance_and_bearing);
    RUN_TEST(test_point_at_distance_due_north_changes_only_latitude);
    return UNITY_END();
}
