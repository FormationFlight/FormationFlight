// Speed autothrottle. Driven through loop(); along-track error is
// engineered precisely by placing the follower a known distance from the
// *computed target* (not the peer) along the course axis, using the same
// slotToLatLon()/calculatePointAtDistance() primitives test_slot_geometry.cpp
// already verified independently.

#include <unity.h>

#include "test_helpers.h"
#include "../../src/lib/GNSS/GNSSManager.h"

static const double PEER_LAT = 37.0;
static const double PEER_LON = -122.0;
static const double COURSE = 0.0; // due north, so along-track == pure north/south

// Places the peer at (PEER_LAT, PEER_LON) heading COURSE, and the follower
// alongTrackErrorM meters *ahead* of the computed target along the course
// axis (negative alongTrackErrorM puts the follower behind it instead) --
// i.e. this directly controls resolveAlongTrackErrorM()'s result.
static void setupWithAlongTrackError(FakePeers &peers, FakeGnss &gnss, double groundSpeedMs, double alongTrackErrorM)
{
    peers.setPeer(0, /*id=*/1, PEER_LAT, PEER_LON, groundSpeedMs, COURSE);

    FollowTarget targetRaw = slotToLatLon((int32_t)lround(PEER_LAT * 1e6), (int32_t)lround(PEER_LON * 1e6),
                                           COURSE, /*long_m=*/-15.0, /*lat_m=*/0.0);
    GNSSLocation targetLoc{};
    targetLoc.lat = (double)targetRaw.lat_1e7 / 1e7;
    targetLoc.lon = (double)targetRaw.lon_1e7 / 1e7;

    // A positive error means the target is *ahead* of the follower (spec
    // §4.2) -- place self at bearing 180 (south) from the target so target
    // is due north of self, distance |alongTrackErrorM|. Negative flips it.
    double bearingFromTarget = (alongTrackErrorM >= 0.0) ? 180.0 : 0.0;
    GNSSLocation self = GNSSManager::calculatePointAtDistance(targetLoc, fabs(alongTrackErrorM), bearingFromTarget);
    gnss.setSelf(self);
}

static void setEngageGvar(FollowManager &fm, int16_t engageIdx = 2)
{
    FollowRuntimeConfig cfg = fm.getConfig();
    cfg.autothrottleEngageGvarIndex = engageIdx;
    String err;
    TEST_ASSERT_TRUE(fm.applyConfig(cfg, &err));
}

static int32_t lastEngageGvar(FakeMsp &msp, uint8_t index)
{
    for (int i = (int)msp.sentGvars.size() - 1; i >= 0; i--)
    {
        if (msp.sentGvars[i].index == index) return msp.sentGvars[i].value;
    }
    TEST_FAIL_MESSAGE("engage GVAR never sent");
    return -1;
}

// ---- Three-way engage gate ----

void test_engage_gate_false_when_not_locked()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers; // no peer -> never locks
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;
    msp.platformType = INAV_PLATFORM_AIRPLANE;
    setEngageGvar(fm);

    followTick(fm);
    TEST_ASSERT_EQUAL(0, lastEngageGvar(msp, 2));
}

void test_engage_gate_false_when_locked_but_not_airplane()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;
    msp.platformType = INAV_PLATFORM_MULTIROTOR;
    setupWithAlongTrackError(peers, gnss, /*groundSpeedMs=*/15.0, /*alongTrackErrorM=*/0.0);
    setEngageGvar(fm);

    followTick(fm);
    TEST_ASSERT_EQUAL(0, lastEngageGvar(msp, 2));
}

void test_engage_gate_false_when_arm_channel_outside_range()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;
    msp.platformType = INAV_PLATFORM_AIRPLANE;
    setupWithAlongTrackError(peers, gnss, 15.0, 0.0);
    setEngageGvar(fm);

    FollowRuntimeConfig cfg = fm.getConfig();
    cfg.autothrottleEnableRcChannel = 6;
    cfg.minTargetSpeedMps = 5;
    cfg.maxTargetSpeedMps = 30;
    String err;
    TEST_ASSERT_TRUE(fm.applyConfig(cfg, &err));
    msp.rcChannelUs[6] = 1000; // below default min threshold (1700)

    followTick(fm);
    TEST_ASSERT_EQUAL(0, lastEngageGvar(msp, 2));
}

void test_engage_gate_true_when_all_three_conditions_met()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;
    msp.platformType = INAV_PLATFORM_AIRPLANE;
    setupWithAlongTrackError(peers, gnss, 15.0, 0.0);
    setEngageGvar(fm);

    followTick(fm);
    TEST_ASSERT_EQUAL(1, lastEngageGvar(msp, 2));
}

void test_engage_drops_immediately_when_one_condition_flips_false()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;
    msp.platformType = INAV_PLATFORM_AIRPLANE;
    setupWithAlongTrackError(peers, gnss, 15.0, 0.0);
    setEngageGvar(fm);

    followTick(fm);
    TEST_ASSERT_EQUAL(1, lastEngageGvar(msp, 2));

    msp.platformType = INAV_PLATFORM_MULTIROTOR; // flip one condition false
    followTick(fm);
    TEST_ASSERT_EQUAL(0, lastEngageGvar(msp, 2)); // drops immediately, no latching
}

// ---- resolveTargetSpeedCmS() kinematic braking law ----

void test_zero_accel_is_pure_feedforward()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;
    msp.platformType = INAV_PLATFORM_AIRPLANE;

    FollowRuntimeConfig cfg = fm.getConfig();
    cfg.minTargetSpeedMps = 5; // wide enough clamp that this test's speeds pass through unclamped
    cfg.maxTargetSpeedMps = 30;
    String err;
    TEST_ASSERT_TRUE(fm.applyConfig(cfg, &err));

    setupWithAlongTrackError(peers, gnss, /*groundSpeedMs=*/15.0, /*alongTrackErrorM=*/40.0);
    // speedCorrectionAccelCmS2 defaults to 0.

    followTick(fm);
    DynamicJsonDocument doc(1024);
    fm.statusJson(&doc);
    TEST_ASSERT_EQUAL_INT32(1500, doc["targetSpeedCmS"].as<int32_t>()); // exactly groundSpeed, error ignored
}

void test_positive_error_adds_correction_negative_subtracts()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;
    msp.platformType = INAV_PLATFORM_AIRPLANE;

    FollowRuntimeConfig cfg = fm.getConfig();
    cfg.speedCorrectionAccelCmS2 = 50;
    cfg.minTargetSpeedMps = 5; // wide enough clamp that this test's speeds pass through unclamped
    cfg.maxTargetSpeedMps = 30;
    String err;
    TEST_ASSERT_TRUE(fm.applyConfig(cfg, &err));

    // Hand-computed: a=50 cm/s^2, d=20m -> errorCm=2000 ->
    // correction = sqrt(2*50*2000) = sqrt(200000) ~= 447.21 cm/s.
    setupWithAlongTrackError(peers, gnss, /*groundSpeedMs=*/15.0, /*alongTrackErrorM=*/20.0);
    followTick(fm);
    DynamicJsonDocument doc1(1024);
    fm.statusJson(&doc1);
    TEST_ASSERT_INT32_WITHIN(2, 1500 + 447, doc1["targetSpeedCmS"].as<int32_t>());

    // Reuse the same (already-locked) instance for the mirrored case --
    // just moves the follower to the other side of the target.
    setupWithAlongTrackError(peers, gnss, /*groundSpeedMs=*/15.0, /*alongTrackErrorM=*/-20.0);
    followTick(fm);
    DynamicJsonDocument doc2(1024);
    fm.statusJson(&doc2);
    TEST_ASSERT_INT32_WITHIN(2, 1500 - 447, doc2["targetSpeedCmS"].as<int32_t>());
}

void test_target_speed_clamps_to_max_and_min()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;
    msp.platformType = INAV_PLATFORM_AIRPLANE;

    FollowRuntimeConfig cfg = fm.getConfig();
    cfg.speedCorrectionAccelCmS2 = 50;
    cfg.maxTargetDistM = 1000.0; // wide enough that a 300m along-track error isn't itself suppressed
    cfg.minTargetSpeedMps = 5;
    cfg.maxTargetSpeedMps = 30;
    String err;
    TEST_ASSERT_TRUE(fm.applyConfig(cfg, &err));

    // a=50 cm/s^2, d=300m -> errorCm=30000 -> correction = sqrt(2*50*30000)
    // = sqrt(3,000,000) ~= 1732 cm/s -> 1500+1732 = 3232, well past
    // maxTargetSpeedMps (30 m/s = 3000 cm/s, set above).
    setupWithAlongTrackError(peers, gnss, 15.0, 300.0);
    followTick(fm);
    DynamicJsonDocument doc1(1024);
    fm.statusJson(&doc1);
    TEST_ASSERT_EQUAL_INT32(3000, doc1["targetSpeedCmS"].as<int32_t>());

    // Mirrored: 1500-1732 clamps to minTargetSpeedMps (5 m/s = 500 cm/s, set above).
    setupWithAlongTrackError(peers, gnss, 15.0, -300.0);
    followTick(fm);
    DynamicJsonDocument doc2(1024);
    fm.statusJson(&doc2);
    TEST_ASSERT_EQUAL_INT32(500, doc2["targetSpeedCmS"].as<int32_t>());
}

// ---- autothrottleArmed() ----

void test_autothrottle_armed_when_channel_unassigned()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;
    msp.platformType = INAV_PLATFORM_AIRPLANE;
    setupWithAlongTrackError(peers, gnss, 15.0, 0.0);
    setEngageGvar(fm); // autothrottleEnableRcChannel stays -1 (default)

    followTick(fm);
    TEST_ASSERT_EQUAL(1, lastEngageGvar(msp, 2));
}

void test_autothrottle_armed_when_assigned_channel_read_fails()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;
    msp.platformType = INAV_PLATFORM_AIRPLANE;
    setupWithAlongTrackError(peers, gnss, 15.0, 0.0);
    setEngageGvar(fm);

    FollowRuntimeConfig cfg = fm.getConfig();
    cfg.autothrottleEnableRcChannel = 6;
    cfg.minTargetSpeedMps = 5;
    cfg.maxTargetSpeedMps = 30;
    String err;
    TEST_ASSERT_TRUE(fm.applyConfig(cfg, &err));
    // Channel 6 deliberately absent from msp.rcChannelUs.

    followTick(fm);
    TEST_ASSERT_EQUAL(1, lastEngageGvar(msp, 2)); // same no-FC fallback as resolveAxisOffset()
}

void test_autothrottle_armed_only_within_threshold_range()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = true;
    msp.platformType = INAV_PLATFORM_AIRPLANE;
    setupWithAlongTrackError(peers, gnss, 15.0, 0.0);
    setEngageGvar(fm);

    FollowRuntimeConfig cfg = fm.getConfig();
    cfg.autothrottleEnableRcChannel = 6; // default range [1700, 2100]
    cfg.minTargetSpeedMps = 5;
    cfg.maxTargetSpeedMps = 30;
    String err;
    TEST_ASSERT_TRUE(fm.applyConfig(cfg, &err));

    msp.rcChannelUs[6] = 1700; // inclusive lower bound
    followTick(fm);
    TEST_ASSERT_EQUAL(1, lastEngageGvar(msp, 2));

    msp.rcChannelUs[6] = 2100; // inclusive upper bound
    followTick(fm);
    TEST_ASSERT_EQUAL(1, lastEngageGvar(msp, 2));

    msp.rcChannelUs[6] = 2101; // just outside
    followTick(fm);
    TEST_ASSERT_EQUAL(0, lastEngageGvar(msp, 2));
}
