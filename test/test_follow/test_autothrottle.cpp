// Speed autothrottle. Driven through service(); along-track error is
// engineered precisely by placing the follower a known distance from the
// *computed target* (not the peer) along the course axis, using the same
// slotToLatLon()/geo::pointAtDistance() primitives test_slot_geometry.cpp
// already verified independently.

#include <unity.h>

#include <cmath>

#include "test_helpers.h"

static const double PEER_LAT = 37.0;
static const double PEER_LON = -122.0;
static const double COURSE = 0.0;  // due north, so along-track == pure north/south

// Places peer uid=1 at (PEER_LAT, PEER_LON) heading COURSE, and the follower
// alongTrackErrorM meters *behind* the computed target along the course axis
// (negative alongTrackErrorM puts the follower ahead of it instead) -- i.e.
// this directly controls resolveAlongTrackErrorM()'s result.
static void setupWithAlongTrackError(FollowHarness& h, double groundSpeedMs,
                                     double alongTrackErrorM) {
    h.setPeer(/*uid=*/1, PEER_LAT, PEER_LON, groundSpeedMs, COURSE);

    const FollowTarget targetRaw =
        slotToLatLon(static_cast<int32_t>(std::lround(PEER_LAT * 1e7)),
                     static_cast<int32_t>(std::lround(PEER_LON * 1e7)), COURSE,
                     /*long_m=*/-15.0, /*lat_m=*/0.0);
    const double targetLat = static_cast<double>(targetRaw.lat_1e7) / 1e7;
    const double targetLon = static_cast<double>(targetRaw.lon_1e7) / 1e7;

    // A positive error means the target is *ahead* of the follower (spec
    // section 4.2) -- place self at bearing 180 (south) from the target so the
    // target is due north of self, distance |alongTrackErrorM|. Negative flips it.
    const double bearingFromTarget = (alongTrackErrorM >= 0.0) ? 180.0 : 0.0;
    double selfLat = 0.0;
    double selfLon = 0.0;
    geo::pointAtDistance(targetLat, targetLon, std::fabs(alongTrackErrorM), bearingFromTarget,
                         selfLat, selfLon);
    h.self.set(selfLat, selfLon);
}

static void setEngageGvar(FollowHarness& h, int16_t engageIdx = 2) {
    FollowConfig cfg = configOf(h);
    cfg.autothrottleEngageGvarIndex = engageIdx;
    const char* err = nullptr;
    TEST_ASSERT_TRUE_MESSAGE(h.apply(cfg, &err), err ? err : "applyConfig failed");
}

static int32_t lastEngageGvar(FakeFc& fc, uint8_t index) {
    for (int i = static_cast<int>(fc.sentGvars.size()) - 1; i >= 0; i--) {
        if (fc.sentGvars[static_cast<size_t>(i)].index == index) {
            return fc.sentGvars[static_cast<size_t>(i)].value;
        }
    }
    TEST_FAIL_MESSAGE("engage GVAR never sent");
    return -1;
}

// ---- Three-way engage gate ----

void test_engage_gate_false_when_not_locked() {
    FollowHarness h;  // no peer -> never locks
    h.fc.gcsNav = true;
    h.fc.platform = FcPlatform::Airplane;
    setEngageGvar(h);

    h.tick();
    TEST_ASSERT_EQUAL(0, lastEngageGvar(h.fc, 2));
}

void test_engage_gate_false_when_locked_but_not_airplane() {
    FollowHarness h;
    h.fc.gcsNav = true;
    h.fc.platform = FcPlatform::Multirotor;
    setupWithAlongTrackError(h, /*groundSpeedMs=*/15.0, /*alongTrackErrorM=*/0.0);
    setEngageGvar(h);

    h.tick();
    TEST_ASSERT_EQUAL(0, lastEngageGvar(h.fc, 2));
}

void test_engage_gate_false_when_arm_channel_outside_range() {
    FollowHarness h;
    h.fc.gcsNav = true;
    h.fc.platform = FcPlatform::Airplane;
    setupWithAlongTrackError(h, 15.0, 0.0);
    setEngageGvar(h);

    FollowConfig cfg = configOf(h);
    cfg.autothrottleEnableRcChannel = 6;
    cfg.minTargetSpeedMps = 5;
    cfg.maxTargetSpeedMps = 30;
    const char* err = nullptr;
    TEST_ASSERT_TRUE(h.apply(cfg, &err));
    h.fc.rc[6] = 1000;  // below default min threshold (1700)

    h.tick();
    TEST_ASSERT_EQUAL(0, lastEngageGvar(h.fc, 2));
}

void test_engage_gate_true_when_all_three_conditions_met() {
    FollowHarness h;
    h.fc.gcsNav = true;
    h.fc.platform = FcPlatform::Airplane;
    setupWithAlongTrackError(h, 15.0, 0.0);
    setEngageGvar(h);

    h.tick();
    TEST_ASSERT_EQUAL(1, lastEngageGvar(h.fc, 2));
}

void test_engage_drops_immediately_when_one_condition_flips_false() {
    FollowHarness h;
    h.fc.gcsNav = true;
    h.fc.platform = FcPlatform::Airplane;
    setupWithAlongTrackError(h, 15.0, 0.0);
    setEngageGvar(h);

    h.tick();
    TEST_ASSERT_EQUAL(1, lastEngageGvar(h.fc, 2));

    h.fc.platform = FcPlatform::Multirotor;  // flip one condition false
    h.tick();
    TEST_ASSERT_EQUAL(0, lastEngageGvar(h.fc, 2));  // drops immediately, no latching
}

// ---- resolveTargetSpeedCmS() kinematic braking law ----

void test_zero_accel_is_pure_feedforward() {
    FollowHarness h;
    h.fc.gcsNav = true;
    h.fc.platform = FcPlatform::Airplane;

    FollowConfig cfg = configOf(h);
    cfg.minTargetSpeedMps = 5;  // wide enough clamp that this test's speeds pass through unclamped
    cfg.maxTargetSpeedMps = 30;
    const char* err = nullptr;
    TEST_ASSERT_TRUE(h.apply(cfg, &err));

    setupWithAlongTrackError(h, /*groundSpeedMs=*/15.0, /*alongTrackErrorM=*/40.0);
    // speedCorrectionAccelCmS2 defaults to 0.

    h.tick();
    FollowStatus s = h.status();
    TEST_ASSERT_TRUE(s.haveLastTarget);
    TEST_ASSERT_EQUAL_INT32(1500, s.targetSpeedCmS);  // exactly groundSpeed, error ignored
}

void test_positive_error_adds_correction_negative_subtracts() {
    FollowHarness h;
    h.fc.gcsNav = true;
    h.fc.platform = FcPlatform::Airplane;

    FollowConfig cfg = configOf(h);
    cfg.speedCorrectionAccelCmS2 = 50;
    cfg.minTargetSpeedMps = 5;  // wide enough clamp that this test's speeds pass through unclamped
    cfg.maxTargetSpeedMps = 30;
    const char* err = nullptr;
    TEST_ASSERT_TRUE(h.apply(cfg, &err));

    // Hand-computed: a=50 cm/s^2, d=20m -> errorCm=2000 ->
    // correction = sqrt(2*50*2000) = sqrt(200000) ~= 447.21 cm/s.
    setupWithAlongTrackError(h, /*groundSpeedMs=*/15.0, /*alongTrackErrorM=*/20.0);
    h.tick();
    FollowStatus s1 = h.status();
    TEST_ASSERT_TRUE(s1.haveLastTarget);
    TEST_ASSERT_INT32_WITHIN(2, 1500 + 447, s1.targetSpeedCmS);

    // Reuse the same (already-locked) instance for the mirrored case --
    // just moves the follower to the other side of the target.
    setupWithAlongTrackError(h, /*groundSpeedMs=*/15.0, /*alongTrackErrorM=*/-20.0);
    h.tick();
    FollowStatus s2 = h.status();
    TEST_ASSERT_TRUE(s2.haveLastTarget);
    TEST_ASSERT_INT32_WITHIN(2, 1500 - 447, s2.targetSpeedCmS);
}

void test_target_speed_clamps_to_max_and_min() {
    FollowHarness h;
    h.fc.gcsNav = true;
    h.fc.platform = FcPlatform::Airplane;

    FollowConfig cfg = configOf(h);
    cfg.speedCorrectionAccelCmS2 = 50;
    cfg.maxTargetDistM = 1000.0;  // wide enough that a 300m along-track error isn't itself suppressed
    cfg.minTargetSpeedMps = 5;
    cfg.maxTargetSpeedMps = 30;
    const char* err = nullptr;
    TEST_ASSERT_TRUE(h.apply(cfg, &err));

    // a=50 cm/s^2, d=300m -> errorCm=30000 -> correction = sqrt(2*50*30000)
    // = sqrt(3,000,000) ~= 1732 cm/s -> 1500+1732 = 3232, well past
    // maxTargetSpeedMps (30 m/s = 3000 cm/s, set above).
    setupWithAlongTrackError(h, 15.0, 300.0);
    h.tick();
    FollowStatus s1 = h.status();
    TEST_ASSERT_TRUE(s1.haveLastTarget);
    TEST_ASSERT_EQUAL_INT32(3000, s1.targetSpeedCmS);

    // Mirrored: 1500-1732 clamps to minTargetSpeedMps (5 m/s = 500 cm/s, set above).
    setupWithAlongTrackError(h, 15.0, -300.0);
    h.tick();
    FollowStatus s2 = h.status();
    TEST_ASSERT_TRUE(s2.haveLastTarget);
    TEST_ASSERT_EQUAL_INT32(500, s2.targetSpeedCmS);
}

// ---- autothrottleArmed() ----

void test_autothrottle_armed_when_channel_unassigned() {
    FollowHarness h;
    h.fc.gcsNav = true;
    h.fc.platform = FcPlatform::Airplane;
    setupWithAlongTrackError(h, 15.0, 0.0);
    setEngageGvar(h);  // autothrottleEnableRcChannel stays -1 (default)

    h.tick();
    TEST_ASSERT_EQUAL(1, lastEngageGvar(h.fc, 2));
}

void test_autothrottle_armed_when_assigned_channel_read_fails() {
    FollowHarness h;
    h.fc.gcsNav = true;
    h.fc.platform = FcPlatform::Airplane;
    setupWithAlongTrackError(h, 15.0, 0.0);
    setEngageGvar(h);

    FollowConfig cfg = configOf(h);
    cfg.autothrottleEnableRcChannel = 6;
    cfg.minTargetSpeedMps = 5;
    cfg.maxTargetSpeedMps = 30;
    const char* err = nullptr;
    TEST_ASSERT_TRUE(h.apply(cfg, &err));
    // Channel 6 deliberately absent from h.fc.rc, so rcChannelUs() fails.

    h.tick();
    TEST_ASSERT_EQUAL(1, lastEngageGvar(h.fc, 2));  // same no-FC fallback as resolveAxisOffset()
}

void test_autothrottle_armed_only_within_threshold_range() {
    FollowHarness h;
    h.fc.gcsNav = true;
    h.fc.platform = FcPlatform::Airplane;
    setupWithAlongTrackError(h, 15.0, 0.0);
    setEngageGvar(h);

    FollowConfig cfg = configOf(h);
    cfg.autothrottleEnableRcChannel = 6;  // default range [1700, 2100]
    cfg.minTargetSpeedMps = 5;
    cfg.maxTargetSpeedMps = 30;
    const char* err = nullptr;
    TEST_ASSERT_TRUE(h.apply(cfg, &err));

    h.fc.rc[6] = 1700;  // inclusive lower bound
    h.tick();
    TEST_ASSERT_EQUAL(1, lastEngageGvar(h.fc, 2));

    h.fc.rc[6] = 2100;  // inclusive upper bound
    h.tick();
    TEST_ASSERT_EQUAL(1, lastEngageGvar(h.fc, 2));

    h.fc.rc[6] = 2101;  // just outside
    h.tick();
    TEST_ASSERT_EQUAL(0, lastEngageGvar(h.fc, 2));
}
