// Simulated peer motion.
//
// These paths are what a Follow controller gets tested against on the bench, so
// a wrong one produces confident, plausible, useless results. Each mode is
// checked against the geometric property that defines it -- a circle stays at
// its radius, a hexagon closes -- rather than against whatever the code happens
// to emit today.

#include <unity.h>

#include <cmath>
#include <cstring>

#include "geo.h"
#include "sim_traffic.h"

using namespace ff;

void setUp() {}
void tearDown() {}

static const double kLat = 37.0;
static const double kLon = -122.0;

static SimPeerConfig makeConfig(SimMode mode, double speed_ms, double radius_m = 100.0) {
    SimPeerConfig c;
    c.uid = 0x5EED0001;
    std::strncpy(c.name, "SIM", kMaxNameLen);
    c.mode = mode;
    c.lat = kLat;
    c.lon = kLon;
    c.alt_m = 100;
    c.speed_ms = speed_ms;
    c.course_deg = 90.0;
    c.radius_m = radius_m;
    return c;
}

static double distanceFromOrigin(const SimPeerState& s) {
    return geo::distanceM(kLat, kLon, s.lat, s.lon);
}

// ---- Static ------------------------------------------------------------------

void test_static_peer_never_moves() {
    const SimPeerConfig c = makeConfig(SimMode::Static, 0.0);
    for (uint32_t t = 0; t < 60000; t += 5000) {
        const SimPeerState s = simPeerAt(c, t);
        TEST_ASSERT_DOUBLE_WITHIN(1e-9, kLat, s.lat);
        TEST_ASSERT_DOUBLE_WITHIN(1e-9, kLon, s.lon);
        TEST_ASSERT_EQUAL_INT16(100, s.alt_m);
        TEST_ASSERT_EQUAL_UINT16(0, s.speed_cms);
        TEST_ASSERT_EQUAL_UINT16(900, s.course_ddeg);
    }
}

// A speed set on a static peer must still not move it: the mode decides.
void test_static_peer_ignores_speed() {
    const SimPeerConfig c = makeConfig(SimMode::Static, 25.0);
    const SimPeerState s = simPeerAt(c, 10000);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, kLat, s.lat);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, kLon, s.lon);
}

// ---- Line --------------------------------------------------------------------

void test_line_travels_at_the_configured_speed_and_course() {
    const SimPeerConfig c = makeConfig(SimMode::Line, 20.0);
    const SimPeerState s = simPeerAt(c, 10000);  // 10 s at 20 m/s = 200 m
    TEST_ASSERT_DOUBLE_WITHIN(0.5, 200.0, distanceFromOrigin(s));
    TEST_ASSERT_DOUBLE_WITHIN(0.1, 90.0, geo::bearingDeg(kLat, kLon, s.lat, s.lon));
    TEST_ASSERT_EQUAL_UINT16(2000, s.speed_cms);
    TEST_ASSERT_EQUAL_UINT16(900, s.course_ddeg);
}

void test_line_distance_is_linear_in_time() {
    const SimPeerConfig c = makeConfig(SimMode::Line, 15.0);
    const double d1 = distanceFromOrigin(simPeerAt(c, 4000));
    const double d2 = distanceFromOrigin(simPeerAt(c, 8000));
    TEST_ASSERT_DOUBLE_WITHIN(0.5, 60.0, d1);
    TEST_ASSERT_DOUBLE_WITHIN(1.0, 2.0 * d1, d2);
}

// ---- Circle ------------------------------------------------------------------

void test_circle_stays_on_its_radius() {
    const SimPeerConfig c = makeConfig(SimMode::Circle, 20.0, 150.0);
    for (uint32_t t = 0; t < 60000; t += 1000) {
        const SimPeerState s = simPeerAt(c, t);
        TEST_ASSERT_DOUBLE_WITHIN(0.5, 150.0, distanceFromOrigin(s));
    }
}

void test_circle_closes_after_one_lap() {
    const double radius = 100.0;
    const double speed = 25.0;
    const SimPeerConfig c = makeConfig(SimMode::Circle, speed, radius);
    // One lap is 2*pi*r metres.
    const double lap_ms = (2.0 * geo::kPi * radius / speed) * 1000.0;

    const SimPeerState start = simPeerAt(c, 0);
    const SimPeerState end = simPeerAt(c, static_cast<uint32_t>(std::lround(lap_ms)));
    TEST_ASSERT_DOUBLE_WITHIN(1.0, 0.0, geo::distanceM(start.lat, start.lon, end.lat, end.lon));
}

// The reported course must be the direction of travel, which for a circle is the
// tangent: 90 degrees off the radial. A follower steering to a radial heading
// would sit permanently rotated out of its slot.
void test_circle_course_is_the_tangent() {
    const SimPeerConfig c = makeConfig(SimMode::Circle, 20.0, 120.0);
    for (uint32_t t = 1000; t < 20000; t += 2500) {
        const SimPeerState s = simPeerAt(c, t);
        const double radial = geo::bearingDeg(kLat, kLon, s.lat, s.lon);
        double delta = static_cast<double>(s.course_ddeg) / 10.0 - radial;
        while (delta < 0.0) delta += 360.0;
        while (delta >= 360.0) delta -= 360.0;
        TEST_ASSERT_DOUBLE_WITHIN(1.0, 90.0, delta);
    }
}

void test_circle_with_zero_radius_does_not_divide_by_zero() {
    const SimPeerConfig c = makeConfig(SimMode::Circle, 20.0, 0.0);
    const SimPeerState s = simPeerAt(c, 5000);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, kLat, s.lat);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, kLon, s.lon);
}

// ---- Hexagon -----------------------------------------------------------------

void test_hex_closes_and_returns_to_its_start_altitude() {
    const double side = 100.0;
    const double speed = 20.0;
    const SimPeerConfig c = makeConfig(SimMode::Hex, speed, side);
    const double lap_ms = (6.0 * side / speed) * 1000.0;

    const SimPeerState start = simPeerAt(c, 0);
    const SimPeerState end = simPeerAt(c, static_cast<uint32_t>(std::lround(lap_ms)));
    TEST_ASSERT_DOUBLE_WITHIN(1.0, 0.0, geo::distanceM(start.lat, start.lon, end.lat, end.lon));
    TEST_ASSERT_INT16_WITHIN(1, start.alt_m, end.alt_m);
    TEST_ASSERT_EQUAL_INT16(100, start.alt_m);
}

// Every vertex of a regular hexagon is one side length from the centre, and no
// point on an edge is further out than that.
void test_hex_never_leaves_its_circumradius() {
    const double side = 80.0;
    const SimPeerConfig c = makeConfig(SimMode::Hex, 20.0, side);
    for (uint32_t t = 0; t < 30000; t += 250) {
        const SimPeerState s = simPeerAt(c, t);
        const double d = distanceFromOrigin(s);
        TEST_ASSERT_TRUE_MESSAGE(d <= side + 1.0, "hex path left its circumradius");
        // The closest approach to the centre is the apothem, side * sqrt(3)/2.
        TEST_ASSERT_TRUE_MESSAGE(d >= side * 0.86 - 1.0, "hex path cut through the centre");
    }
}

void test_hex_climbs_to_a_peak_at_the_half_way_vertex() {
    const double side = 100.0;
    const double speed = 20.0;
    const SimPeerConfig c = makeConfig(SimMode::Hex, speed, side);
    const double lap_ms = (6.0 * side / speed) * 1000.0;

    const SimPeerState mid = simPeerAt(c, static_cast<uint32_t>(std::lround(lap_ms / 2.0)));
    TEST_ASSERT_INT16_WITHIN(2, static_cast<int16_t>(100 + kSimHexClimbM), mid.alt_m);

    // And it is a ramp, not a step: a quarter of the way round is about halfway
    // up the climb.
    const SimPeerState quarter = simPeerAt(c, static_cast<uint32_t>(std::lround(lap_ms / 4.0)));
    TEST_ASSERT_INT16_WITHIN(3, static_cast<int16_t>(100 + kSimHexClimbM / 2.0), quarter.alt_m);
}

// The heading must change in six discrete steps of 60 degrees, which is the
// property that makes this path worth flying against: a follower has to cope
// with the leader's track rotating abruptly.
void test_hex_course_changes_by_sixty_degrees_per_leg() {
    const double side = 100.0;
    const double speed = 20.0;
    const SimPeerConfig c = makeConfig(SimMode::Hex, speed, side);
    const double leg_ms = (side / speed) * 1000.0;

    double previous = -1.0;
    for (int leg = 0; leg < 6; leg++) {
        // Sample the middle of each leg, away from the vertex transitions.
        const uint32_t t = static_cast<uint32_t>(std::lround(leg_ms * (leg + 0.5)));
        const double course = static_cast<double>(simPeerAt(c, t).course_ddeg) / 10.0;
        if (previous >= 0.0) {
            double delta = course - previous;
            while (delta < 0.0) delta += 360.0;
            TEST_ASSERT_DOUBLE_WITHIN(0.5, 60.0, delta);
        }
        previous = course;
    }
}

void test_hex_wraps_across_many_laps() {
    const double side = 50.0;
    const double speed = 25.0;
    const SimPeerConfig c = makeConfig(SimMode::Hex, speed, side);
    const double lap_ms = (6.0 * side / speed) * 1000.0;

    const SimPeerState first = simPeerAt(c, 1234);
    const SimPeerState later =
        simPeerAt(c, static_cast<uint32_t>(std::lround(lap_ms * 7.0)) + 1234);
    TEST_ASSERT_DOUBLE_WITHIN(1.5, 0.0, geo::distanceM(first.lat, first.lon, later.lat, later.lon));
    TEST_ASSERT_INT16_WITHIN(2, first.alt_m, later.alt_m);
}

// ---- Packet conversion -------------------------------------------------------

void test_position_packet_converts_units_and_claims_a_fix() {
    SimPeerConfig c = makeConfig(SimMode::Static, 0.0);
    SimPeerState s;
    s.lat = 37.12345678;
    s.lon = -122.87654321;
    s.alt_m = 250;
    s.speed_cms = 1234;
    s.course_ddeg = 1800;

    const PositionPacket p = simPositionPacket(c, s);
    TEST_ASSERT_EQUAL_UINT32(0x5EED0001, p.uid);
    TEST_ASSERT_EQUAL_INT32(371234568, p.lat);
    TEST_ASSERT_EQUAL_INT32(-1228765432, p.lon);
    TEST_ASSERT_EQUAL_INT16(250, p.alt_m);
    TEST_ASSERT_EQUAL_UINT16(1234, p.speed_cms);
    TEST_ASSERT_EQUAL_UINT16(1800, p.course_ddeg);
    // Without this flag Follow refuses to chase the peer, which would make the
    // simulator useless for the one thing it exists to test.
    TEST_ASSERT_TRUE((p.flags & POSITION_FLAG_HAS_FIX) != 0);
}

// A course of exactly 360 must report as 0, not as an out-of-range 3600.
void test_course_wraps_at_three_sixty() {
    SimPeerConfig c = makeConfig(SimMode::Static, 0.0);
    c.course_deg = 360.0;
    TEST_ASSERT_EQUAL_UINT16(0, simPeerAt(c, 0).course_ddeg);
    c.course_deg = -90.0;
    TEST_ASSERT_EQUAL_UINT16(2700, simPeerAt(c, 0).course_ddeg);
    c.course_deg = 725.0;
    TEST_ASSERT_EQUAL_UINT16(50, simPeerAt(c, 0).course_ddeg);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_static_peer_never_moves);
    RUN_TEST(test_static_peer_ignores_speed);
    RUN_TEST(test_line_travels_at_the_configured_speed_and_course);
    RUN_TEST(test_line_distance_is_linear_in_time);
    RUN_TEST(test_circle_stays_on_its_radius);
    RUN_TEST(test_circle_closes_after_one_lap);
    RUN_TEST(test_circle_course_is_the_tangent);
    RUN_TEST(test_circle_with_zero_radius_does_not_divide_by_zero);
    RUN_TEST(test_hex_closes_and_returns_to_its_start_altitude);
    RUN_TEST(test_hex_never_leaves_its_circumradius);
    RUN_TEST(test_hex_climbs_to_a_peak_at_the_half_way_vertex);
    RUN_TEST(test_hex_course_changes_by_sixty_degrees_per_leg);
    RUN_TEST(test_hex_wraps_across_many_laps);
    RUN_TEST(test_position_packet_converts_units_and_claims_a_fix);
    RUN_TEST(test_course_wraps_at_three_sixty);
    return UNITY_END();
}
