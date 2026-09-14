// RC axis mapping (resolveAxisOffset()) and the pre-arm check. Both
// observed via service()'s pre-arm candidate exposure (status()'s
// preArmCandidateOffset/rcPreArmCheckFailed), which is read-only and
// gate/lock-independent -- no peer or active gate needed, just
// fc->armed()==false (disarmed) and an RC channel assigned.

#include <unity.h>

#include "test_helpers.h"

// Isolates the longitudinal axis: only rcLongChannel is assigned, so
// latM/vertM stay pinned at their static defaults (0, 10) and don't
// interfere with the reading under test.
static void assignLongChannelOnly(FollowHarness& h, int16_t channel) {
    FollowConfig cfg = configOf(h);
    cfg.rcLongChannel = channel;
    const char* err = nullptr;
    TEST_ASSERT_TRUE(h.apply(cfg, &err));
}

static double preArmLongM(const FollowHarness& h) {
    FollowStatus s = h.status();
    TEST_ASSERT_TRUE(s.havePreArmCandidateOffset);
    return s.preArmCandidateOffset.longitudinal_m;
}

// ---- §4.7 resolveAxisOffset() ----

void test_axis_offset_no_channel_assigned_returns_configured_unchanged() {
    FollowHarness h;
    h.fc.armedState = false;  // disarmed
    h.fc.gcsNav = false;      // pre-arm block runs independent of the follow gate

    // Assign a different axis (lat) so the pre-arm block still runs, while
    // rcLongChannel stays -1 (unassigned) -- the axis under test.
    FollowConfig cfg = configOf(h);
    cfg.rcLatChannel = 5;
    const char* err = nullptr;
    TEST_ASSERT_TRUE(h.apply(cfg, &err));

    h.tick();
    TEST_ASSERT_EQUAL_DOUBLE(-15.0, preArmLongM(h));  // ofsLongM's default, untouched
}

void test_axis_offset_fc_read_failure_falls_back_to_configured() {
    FollowHarness h;
    h.fc.armedState = false;
    h.fc.gcsNav = false;
    assignLongChannelOnly(h, 5);
    // Channel 5 deliberately absent from fc.rc -> rcChannelUs() returns false.

    h.tick();
    TEST_ASSERT_EQUAL_DOUBLE(-15.0, preArmLongM(h));
}

void test_axis_offset_center_maps_to_zero() {
    FollowHarness h;
    h.fc.armedState = false;
    h.fc.gcsNav = false;
    assignLongChannelOnly(h, 5);
    h.fc.rc[5] = 1500;

    h.tick();
    TEST_ASSERT_EQUAL_DOUBLE(0.0, preArmLongM(h));
}

void test_axis_offset_full_deflection_maps_to_plus_and_minus_gap() {
    FollowHarness h;
    h.fc.armedState = false;
    h.fc.gcsNav = false;
    assignLongChannelOnly(h, 5);  // gap = |ofsLongM| = 15

    h.fc.rc[5] = 2000;
    h.tick();
    TEST_ASSERT_EQUAL_DOUBLE(15.0, preArmLongM(h));

    h.fc.rc[5] = 1000;
    h.tick();
    TEST_ASSERT_EQUAL_DOUBLE(-15.0, preArmLongM(h));
}

void test_axis_offset_out_of_range_us_clamps_to_nearest_endpoint() {
    FollowHarness h;
    h.fc.armedState = false;
    h.fc.gcsNav = false;
    assignLongChannelOnly(h, 5);

    h.fc.rc[5] = 2500;  // above 2000
    h.tick();
    TEST_ASSERT_EQUAL_DOUBLE(15.0, preArmLongM(h));

    h.fc.rc[5] = 500;  // below 1000
    h.tick();
    TEST_ASSERT_EQUAL_DOUBLE(-15.0, preArmLongM(h));
}

// ---- §4.9 pre-arm check ----

void test_prearm_candidate_computed_every_cycle_while_disarmed_with_axis_assigned() {
    FollowHarness h;
    h.fc.armedState = false;
    h.fc.gcsNav = false;
    assignLongChannelOnly(h, 5);
    h.fc.rc[5] = 1500;

    h.tick();
    FollowStatus s = h.status();
    TEST_ASSERT_TRUE(s.havePreArmCandidateOffset);
}

void test_prearm_forced_false_and_absent_while_armed_regardless_of_assignment() {
    FollowHarness h;
    h.fc.armedState = true;  // armed
    h.fc.gcsNav = false;
    assignLongChannelOnly(h, 5);
    h.fc.rc[5] = 1500;  // would fail the check if evaluated (center != -15 default)

    h.tick();
    FollowStatus s = h.status();
    TEST_ASSERT_FALSE(s.havePreArmCandidateOffset);
    TEST_ASSERT_FALSE(s.rcPreArmCheckFailed);
}

void test_prearm_failed_flag_is_not_sticky_across_arm_transition() {
    FollowHarness h;
    h.fc.armedState = false;
    h.fc.gcsNav = false;
    assignLongChannelOnly(h, 5);
    h.fc.rc[5] = 1500;  // center != default -15 -> fails

    h.tick();
    FollowStatus s1 = h.status();
    TEST_ASSERT_TRUE(s1.rcPreArmCheckFailed);

    h.fc.armedState = true;  // arm
    h.tick();
    FollowStatus s2 = h.status();
    TEST_ASSERT_FALSE(s2.rcPreArmCheckFailed);  // not stale/sticky
}

void test_prearm_center_with_nonzero_default_fails_check() {
    FollowHarness h;
    h.fc.armedState = false;
    h.fc.gcsNav = false;
    assignLongChannelOnly(h, 5);
    h.fc.rc[5] = 1500;  // center -> candidate longM = 0, default is -15

    h.tick();
    FollowStatus s = h.status();
    TEST_ASSERT_TRUE(s.rcPreArmCheckFailed);
}

void test_prearm_matching_static_default_passes_check() {
    FollowHarness h;
    h.fc.armedState = false;
    h.fc.gcsNav = false;
    assignLongChannelOnly(h, 5);
    h.fc.rc[5] = 1000;  // full down -> candidate longM = -15, exact default match

    h.tick();
    FollowStatus s = h.status();
    TEST_ASSERT_FALSE(s.rcPreArmCheckFailed);
}
