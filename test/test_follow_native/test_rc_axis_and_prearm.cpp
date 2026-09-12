// RC axis mapping (resolveAxisOffset()) and the pre-arm check. Both
// observed via loop()'s pre-arm candidate exposure (statusJson()'s
// preArmCandidateOffset/rcPreArmCheckFailed),
// which is read-only and gate/lock-independent -- no peer or active gate
// needed, just msp->getState()==0 (disarmed) and an RC channel assigned.

#include <unity.h>

#include "test_helpers.h"

// Isolates the longitudinal axis: only rcLongChannel is assigned, so
// latM/vertM stay pinned at their static defaults (0, 10) and don't
// interfere with the reading under test.
static void assignLongChannelOnly(FollowManager &fm, int16_t channel)
{
    FollowRuntimeConfig cfg = fm.getConfig();
    cfg.rcLongChannel = channel;
    String err;
    TEST_ASSERT_TRUE(fm.applyConfig(cfg, &err));
}

static double preArmLongM(FollowManager &fm)
{
    DynamicJsonDocument doc(1024);
    fm.statusJson(&doc);
    TEST_ASSERT_TRUE(doc["preArmCandidateOffset"].is<JsonObject>());
    return doc["preArmCandidateOffset"]["longM"].as<double>();
}

// ---- §4.7 resolveAxisOffset() ----

void test_axis_offset_no_channel_assigned_returns_configured_unchanged()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.state = 0; // disarmed

    // Assign a different axis (lat) so the pre-arm block still runs, while
    // rcLongChannel stays -1 (unassigned) -- the axis under test.
    FollowRuntimeConfig cfg = fm.getConfig();
    cfg.rcLatChannel = 5;
    String err;
    TEST_ASSERT_TRUE(fm.applyConfig(cfg, &err));

    followTick(fm);
    TEST_ASSERT_EQUAL_DOUBLE(-15.0, preArmLongM(fm)); // ofsLongM's default, untouched
}

void test_axis_offset_msp_read_failure_falls_back_to_configured()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.state = 0;
    assignLongChannelOnly(fm, 5);
    // Channel 5 deliberately absent from msp.rcChannelUs -> getRcChannelUs() returns false.

    followTick(fm);
    TEST_ASSERT_EQUAL_DOUBLE(-15.0, preArmLongM(fm));
}

void test_axis_offset_center_maps_to_zero()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.state = 0;
    assignLongChannelOnly(fm, 5);
    msp.rcChannelUs[5] = 1500;

    followTick(fm);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, preArmLongM(fm));
}

void test_axis_offset_full_deflection_maps_to_plus_and_minus_gap()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.state = 0;
    assignLongChannelOnly(fm, 5); // gap = |ofsLongM| = 15

    msp.rcChannelUs[5] = 2000;
    followTick(fm);
    TEST_ASSERT_EQUAL_DOUBLE(15.0, preArmLongM(fm));

    msp.rcChannelUs[5] = 1000;
    followTick(fm);
    TEST_ASSERT_EQUAL_DOUBLE(-15.0, preArmLongM(fm));
}

void test_axis_offset_out_of_range_us_clamps_to_nearest_endpoint()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.state = 0;
    assignLongChannelOnly(fm, 5);

    msp.rcChannelUs[5] = 2500; // above 2000
    followTick(fm);
    TEST_ASSERT_EQUAL_DOUBLE(15.0, preArmLongM(fm));

    msp.rcChannelUs[5] = 500; // below 1000
    followTick(fm);
    TEST_ASSERT_EQUAL_DOUBLE(-15.0, preArmLongM(fm));
}

// ---- §4.9 pre-arm check ----

void test_prearm_candidate_computed_every_cycle_while_disarmed_with_axis_assigned()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.state = 0;
    assignLongChannelOnly(fm, 5);
    msp.rcChannelUs[5] = 1500;

    followTick(fm);
    DynamicJsonDocument doc(1024);
    fm.statusJson(&doc);
    TEST_ASSERT_TRUE(doc["preArmCandidateOffset"].is<JsonObject>());
}

void test_prearm_forced_false_and_absent_while_armed_regardless_of_assignment()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.state = 1; // armed
    assignLongChannelOnly(fm, 5);
    msp.rcChannelUs[5] = 1500; // would fail the check if evaluated (center != -15 default)

    followTick(fm);
    DynamicJsonDocument doc(1024);
    fm.statusJson(&doc);
    TEST_ASSERT_FALSE(doc["preArmCandidateOffset"].is<JsonObject>());
    TEST_ASSERT_FALSE(doc["rcPreArmCheckFailed"].as<bool>());
}

void test_prearm_failed_flag_is_not_sticky_across_arm_transition()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.state = 0;
    assignLongChannelOnly(fm, 5);
    msp.rcChannelUs[5] = 1500; // center != default -15 -> fails

    followTick(fm);
    DynamicJsonDocument doc1(1024);
    fm.statusJson(&doc1);
    TEST_ASSERT_TRUE(doc1["rcPreArmCheckFailed"].as<bool>());

    msp.state = 1; // arm
    followTick(fm);
    DynamicJsonDocument doc2(1024);
    fm.statusJson(&doc2);
    TEST_ASSERT_FALSE(doc2["rcPreArmCheckFailed"].as<bool>()); // not stale/sticky
}

void test_prearm_center_with_nonzero_default_fails_check()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.state = 0;
    assignLongChannelOnly(fm, 5);
    msp.rcChannelUs[5] = 1500; // center -> candidate longM = 0, default is -15

    followTick(fm);
    DynamicJsonDocument doc(1024);
    fm.statusJson(&doc);
    TEST_ASSERT_TRUE(doc["rcPreArmCheckFailed"].as<bool>());
}

void test_prearm_matching_static_default_passes_check()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.state = 0;
    assignLongChannelOnly(fm, 5);
    msp.rcChannelUs[5] = 1000; // full down -> candidate longM = -15, exact default match

    followTick(fm);
    DynamicJsonDocument doc(1024);
    fm.statusJson(&doc);
    TEST_ASSERT_FALSE(doc["rcPreArmCheckFailed"].as<bool>());
}
