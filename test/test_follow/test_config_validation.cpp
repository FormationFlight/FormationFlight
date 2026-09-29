// applyConfig() validation, and the shape of the status snapshot.
//
// Persistence moved out of Follow entirely: the whole node configuration, this
// block included, is one JSON document owned by ff_core/config.h and tested in
// test_config. What is left here is the rule set applyConfig() enforces.

#include <unity.h>

#include <cstdio>
#include <cstring>

#include "test_helpers.h"

// ---- Section 4.13 applyConfig() validation: every rejection rule,
// boundary-fail and boundary-pass, table-driven. ----

struct ConfigCase {
    const char* name;
    void (*mutate)(FollowConfig&);
    bool expectValid;
};

// targetUid has no range rule in v2 (any 32-bit UID is a valid target, 0 =
// first active), so the legacy targetPeer=NODES_MAX(+1) rows are gone.
// clang-format off
static const ConfigCase kConfigCases[] = {
    {"emitHz=0 fails",                 [](FollowConfig& c) { c.emitHz = 0; },                 false},
    {"emitHz=1 passes",                [](FollowConfig& c) { c.emitHz = 1; },                 true},
    {"peerTimeoutMs=0 fails",          [](FollowConfig& c) { c.peerTimeoutMs = 0; },          false},
    {"peerTimeoutMs=1 passes",         [](FollowConfig& c) { c.peerTimeoutMs = 1; },          true},
    {"minSepM=-1 fails",               [](FollowConfig& c) { c.minSepM = -1; },               false},
    {"minVSepM=-1 fails",              [](FollowConfig& c) { c.minVSepM = -1; },              false},
    {"minAltM=-1 fails",               [](FollowConfig& c) { c.minAltM = -1; },               false},
    {"minSepM/minVSepM/minAltM=0 passes", [](FollowConfig& c) { c.minSepM = 0; c.minVSepM = 0; c.minAltM = 0; }, true},
    {"maxTargetDistM=0 fails",         [](FollowConfig& c) { c.maxTargetDistM = 0; },         false},
    {"maxTargetDistM=0.001 passes",    [](FollowConfig& c) { c.maxTargetDistM = 0.001; },     true},
    {"minCourseSpeed=-1 fails",        [](FollowConfig& c) { c.minCourseSpeed = -1; },        false},
    {"minCourseSpeed=0 passes",        [](FollowConfig& c) { c.minCourseSpeed = 0; },         true},
    {"statusGvarIndex=-2 fails",       [](FollowConfig& c) { c.statusGvarIndex = -2; },       false},
    {"statusGvarIndex=8 fails",        [](FollowConfig& c) { c.statusGvarIndex = 8; },        false},
    {"statusGvarIndex=-1 passes",      [](FollowConfig& c) { c.statusGvarIndex = -1; },       true},
    {"statusGvarIndex=7 passes",       [](FollowConfig& c) { c.statusGvarIndex = 7; },        true},
    {"conditionFlagsGvarIndex=-2 fails", [](FollowConfig& c) { c.conditionFlagsGvarIndex = -2; }, false},
    {"conditionFlagsGvarIndex=8 fails", [](FollowConfig& c) { c.conditionFlagsGvarIndex = 8; }, false},
    {"conditionFlagsGvarIndex=7 passes", [](FollowConfig& c) { c.conditionFlagsGvarIndex = 7; }, true},
    {"rcLongChannel=0 fails",          [](FollowConfig& c) { c.rcLongChannel = 0; },          false},
    {"rcLongChannel=17 fails",         [](FollowConfig& c) { c.rcLongChannel = 17; },         false},
    {"rcLongChannel=1 passes",         [](FollowConfig& c) { c.rcLongChannel = 1; },          true},
    {"rcLongChannel=16 passes",        [](FollowConfig& c) { c.rcLongChannel = 16; },         true},
    {"rcLatChannel=0 fails",           [](FollowConfig& c) { c.rcLatChannel = 0; },           false},
    {"rcLatChannel=16 passes",         [](FollowConfig& c) { c.rcLatChannel = 16; },          true},
    {"rcVertChannel=0 fails",          [](FollowConfig& c) { c.rcVertChannel = 0; },          false},
    {"rcVertChannel=16 passes",        [](FollowConfig& c) { c.rcVertChannel = 16; },         true},
    {"targetSpeedGvarIndex=8 fails",   [](FollowConfig& c) { c.targetSpeedGvarIndex = 8; },   false},
    {"targetSpeedGvarIndex=7 passes",  [](FollowConfig& c) { c.targetSpeedGvarIndex = 7; },   true},
    {"autothrottleEngageGvarIndex=8 fails", [](FollowConfig& c) { c.autothrottleEngageGvarIndex = 8; }, false},
    {"autothrottleEngageGvarIndex=7 passes", [](FollowConfig& c) { c.autothrottleEngageGvarIndex = 7; }, true},
    {"autothrottleEnableRcChannel=0 fails", [](FollowConfig& c) { c.autothrottleEnableRcChannel = 0; }, false},
    {"autothrottleEnableRcChannel=16 passes", [](FollowConfig& c) { c.autothrottleEnableRcChannel = 16; c.minTargetSpeedMps = 5; c.maxTargetSpeedMps = 30; }, true},
    {"disarmed with default (0/0) target speeds passes", [](FollowConfig& c) { (void)c; }, true},
    {"armed with default (0/0) target speeds fails", [](FollowConfig& c) { c.autothrottleEnableRcChannel = 6; }, false},
    {"armed, minTargetSpeedMps=0 fails", [](FollowConfig& c) { c.autothrottleEnableRcChannel = 6; c.minTargetSpeedMps = 0; c.maxTargetSpeedMps = 10; }, false},
    {"armed, minTargetSpeedMps=-1 fails", [](FollowConfig& c) { c.autothrottleEnableRcChannel = 6; c.minTargetSpeedMps = -1; c.maxTargetSpeedMps = 10; }, false},
    {"armed, maxTargetSpeedMps==minTargetSpeedMps fails", [](FollowConfig& c) { c.autothrottleEnableRcChannel = 6; c.minTargetSpeedMps = 10; c.maxTargetSpeedMps = 10; }, false},
    {"armed, maxTargetSpeedMps>minTargetSpeedMps>0 passes", [](FollowConfig& c) { c.autothrottleEnableRcChannel = 6; c.minTargetSpeedMps = 10; c.maxTargetSpeedMps = 10.1; }, true},
    {"speedCorrectionAccelCmS2=-1 fails", [](FollowConfig& c) { c.speedCorrectionAccelCmS2 = -1; }, false},
    {"speedCorrectionAccelCmS2=0 passes", [](FollowConfig& c) { c.speedCorrectionAccelCmS2 = 0; }, true},
    {"offset magnitude just under minSepM fails", [](FollowConfig& c) { c.ofsLongM = -2; c.ofsLatM = 0; c.ofsVertM = 0; c.minSepM = 8; }, false},
    {"offset magnitude at minSepM passes", [](FollowConfig& c) { c.ofsLongM = -8; c.ofsLatM = 0; c.ofsVertM = 0; c.minSepM = 8; }, true},
    // These overlap/ordering checks are also enforced client-side in the web
    // UI's validateConfig(), so a raw POST bypassing the web UI still gets the
    // same guarantee.
    {"statusGvarIndex==conditionFlagsGvarIndex fails", [](FollowConfig& c) { c.statusGvarIndex = 1; c.conditionFlagsGvarIndex = 1; }, false},
    {"rcLongChannel==rcLatChannel fails", [](FollowConfig& c) { c.rcLongChannel = 5; c.rcLatChannel = 5; }, false},
    {"autothrottleEnableRcChannel overlaps rc axis channel fails", [](FollowConfig& c) { c.rcLongChannel = 5; c.autothrottleEnableRcChannel = 5; }, false},
    {"autothrottleEnableMaxThresholdUs<=MinThresholdUs fails", [](FollowConfig& c) { c.autothrottleEnableMinThresholdUs = 2100; c.autothrottleEnableMaxThresholdUs = 1700; }, false},
};
// clang-format on

void test_applyConfig_validation_rules_table() {
    FollowHarness h;

    for (const auto& tc : kConfigCases) {
        // Default-constructed, not configOf(h) -- the latter reflects whatever
        // the last *accepted* case in this loop left live, so an earlier
        // passing case (e.g. statusGvarIndex=7) would otherwise leak into a
        // later one (e.g. conditionFlagsGvarIndex=7) and the GVAR-uniqueness
        // rule would reject it for a reason unrelated to what this case is
        // actually testing.
        FollowConfig cfg;
        tc.mutate(cfg);
        const char* err = nullptr;
        const bool ok = h.apply(cfg, &err);
        if (ok != tc.expectValid) {
            char msg[256];
            std::snprintf(msg, sizeof(msg), "%s %s%s%s", tc.name,
                          tc.expectValid ? "expected valid" : "expected invalid",
                          err ? ": " : "", err ? err : "");
            TEST_FAIL_MESSAGE(msg);
        }
        // A rejection must always come with a reason; an acceptance never does.
        TEST_ASSERT_EQUAL_MESSAGE(!tc.expectValid, err != nullptr, tc.name);
    }
}

void test_rejected_applyConfig_leaves_live_config_untouched() {
    FollowHarness h;

    const FollowConfig before = configOf(h);

    FollowConfig bad = before;
    bad.emitHz = 0;          // guaranteed rejection
    bad.ofsLongM = 12345.0;  // a field that would be obviously different if it leaked through
    const char* err = nullptr;
    TEST_ASSERT_FALSE(h.apply(bad, &err));
    TEST_ASSERT_NOT_NULL(err);

    const FollowConfig after = configOf(h);
    TEST_ASSERT_EQUAL_DOUBLE(before.ofsLongM, after.ofsLongM);
    TEST_ASSERT_EQUAL(before.emitHz, after.emitHz);
}

// ---- Section 4.14 status contract: conditional fields ----

void test_status_conditional_fields_present_and_absent_as_documented() {
    FollowHarness h;  // no peer -> haveLastTarget/havePreArmCandidateOffset stay false
    h.fc.gcsNav = false;

    h.tick();
    const FollowStatus absent = h.status();
    TEST_ASSERT_FALSE(absent.haveLastTarget);  // covers lastTarget and liveOffset alike
    TEST_ASSERT_FALSE(absent.havePreArmCandidateOffset);
    TEST_ASSERT_FALSE(absent.haveStatusGvarValue);  // index disabled (-1)

    // Now drive to a fully-locked, target-emitted, pre-arm-computed state.
    FollowConfig cfg = configOf(h);
    cfg.rcLongChannel = 5;  // so havePreArmCandidateOffset can go true while disarmed
    cfg.statusGvarIndex = 1;
    const char* err = nullptr;
    TEST_ASSERT_TRUE(h.apply(cfg, &err));
    h.fc.armedState = false;  // disarmed -> pre-arm candidate computed
    h.fc.gcsNav = true;
    h.setPeer(/*uid=*/1, 37.0, -122.0, 10.0, 0.0);
    h.self.set(37.0, -122.0);

    h.tick();
    const FollowStatus present = h.status();
    TEST_ASSERT_TRUE(present.haveLastTarget);
    TEST_ASSERT_TRUE(present.havePreArmCandidateOffset);
    TEST_ASSERT_TRUE(present.haveStatusGvarValue);
    TEST_ASSERT_EQUAL(FOLLOW_LOCK_LOCKED, present.state);
    TEST_ASSERT_EQUAL_INT32(2, present.statusGvarValue);  // LOCKED
    // liveOffset is meaningful once haveLastTarget: the static default slot.
    TEST_ASSERT_EQUAL_DOUBLE(-15.0, present.liveOffset.longitudinal_m);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, present.liveOffset.lateral_m);
    TEST_ASSERT_EQUAL_DOUBLE(10.0, present.liveOffset.vertical_m);
}
