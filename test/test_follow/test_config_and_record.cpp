// applyConfig() validation + FollowRecord persistence round-trip (via the
// rate-limited requestSave()/loadRecord() pair and the pure
// followToRecord()/followFromRecord() codec), plus FollowStatus's
// conditional-field contract.

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

// ---- Record round-trip (via the public requestSave()/loadRecord(), which
// internally exercise followToRecord()/followFromRecord()) ----

void test_record_round_trip_preserves_fields_with_documented_rounding() {
    FollowHarness h;

    FollowConfig cfg = configOf(h);
    cfg.ofsLongM = 15.6;           // lround -> 16
    cfg.ofsLatM = -15.6;           // lround -> -16 (away from zero)
    cfg.minSepM = 3.4;             // lround -> 3
    cfg.minVSepM = 0;
    cfg.headingDeg = 99.5;         // lround -> 100
    cfg.minTargetSpeedMps = 5.5;   // lround -> 6
    cfg.maxTargetSpeedMps = 30.5;  // lround -> 31 (still > minTargetSpeedMps after rounding, irrelevant to the raw double stored pre-round anyway)
    cfg.targetUid = 0xA1B2C3D4u;   // a full 32-bit UID, so a narrowing bug can't hide behind the 0 default
    cfg.emitHz = 5;
    const char* err = nullptr;
    TEST_ASSERT_TRUE(h.apply(cfg, &err));

    FollowRecord rec{};
    const char* saveErr = nullptr;
    TEST_ASSERT_TRUE(h.ctl.requestSave(h.now, rec, &saveErr));
    TEST_ASSERT_EQUAL_UINT16(kFollowRecordVersion, rec.version);

    FollowHarness h2;  // fresh instance, compile-time defaults
    TEST_ASSERT_TRUE(h2.ctl.loadRecord(rec));
    const FollowConfig loaded = configOf(h2);

    TEST_ASSERT_EQUAL_DOUBLE(16.0, loaded.ofsLongM);
    TEST_ASSERT_EQUAL_DOUBLE(-16.0, loaded.ofsLatM);
    TEST_ASSERT_EQUAL_DOUBLE(3.0, loaded.minSepM);
    TEST_ASSERT_EQUAL_DOUBLE(100.0, loaded.headingDeg);
    TEST_ASSERT_EQUAL_DOUBLE(6.0, loaded.minTargetSpeedMps);
    TEST_ASSERT_EQUAL_DOUBLE(31.0, loaded.maxTargetSpeedMps);
    // Non-fractional integer fields carry through unchanged.
    TEST_ASSERT_EQUAL_UINT32(cfg.targetUid, loaded.targetUid);
    TEST_ASSERT_EQUAL_UINT16(cfg.emitHz, loaded.emitHz);
}

void test_record_version_mismatch_is_rejected_and_keeps_defaults() {
    FollowHarness h;

    // Corrupt/uninitialized store: a record whose version doesn't match what
    // loadRecord() expects.
    FollowRecord bad{};
    bad.version = 0;  // never a real kFollowRecordVersion value
    bad.ofsLongM = 42;  // would be obvious if a stale record were applied anyway

    TEST_ASSERT_FALSE(h.ctl.loadRecord(bad));

    // Compile-time default untouched, not a crash or a partially-applied
    // garbage record.
    TEST_ASSERT_EQUAL_DOUBLE(-15.0, configOf(h).ofsLongM);
}

void test_record_save_rate_limited_second_call_fails_first_persists() {
    FollowHarness h;

    FollowConfig cfg1 = configOf(h);
    cfg1.ofsLongM = -8.0;  // still geometry-sane against default minSepM(8)
    const char* err = nullptr;
    TEST_ASSERT_TRUE(h.apply(cfg1, &err));
    FollowRecord rec1{};
    const char* saveErr1 = nullptr;
    TEST_ASSERT_TRUE(h.ctl.requestSave(h.now, rec1, &saveErr1));  // first save: no prior commit, always allowed

    FollowConfig cfg2 = configOf(h);
    cfg2.ofsLongM = -9.0;
    TEST_ASSERT_TRUE(h.apply(cfg2, &err));
    FollowRecord rec2{};
    const char* saveErr2 = nullptr;
    TEST_ASSERT_FALSE(h.ctl.requestSave(h.now, rec2, &saveErr2));  // same instant -> within kFollowSaveMinIntervalMs
    TEST_ASSERT_NOT_NULL(saveErr2);

    FollowHarness h2;
    TEST_ASSERT_TRUE(h2.ctl.loadRecord(rec1));
    TEST_ASSERT_EQUAL_DOUBLE(-8.0, configOf(h2).ofsLongM);  // first save's data, not the second (rejected) one

    // Once the minimum interval has elapsed since the last *successful* save,
    // a save goes through again and carries the current (second) config.
    h.now += kFollowSaveMinIntervalMs;
    FollowRecord rec3{};
    const char* saveErr3 = nullptr;
    TEST_ASSERT_TRUE(h.ctl.requestSave(h.now, rec3, &saveErr3));
    TEST_ASSERT_EQUAL_INT16(-9, rec3.ofsLongM);
}

// ---- Pure codec: every persisted field survives followToRecord() ->
// followFromRecord(). Driven by the FOLLOW_CONFIG_*_FIELDS X-macros so a field
// added to the codec later is covered here automatically (and one added to
// FollowConfig but *not* the codec shows up as a missing X-macro entry). ----

void test_record_codec_carries_every_field() {
    const FollowConfig defaults;
    FollowConfig cfg;

    // Distinct, non-default integer values everywhere. The codec is pure, so
    // applyConfig()'s validity rules don't matter here; only the values do.
    // DIRECT fields count up from 1, ROUNDED (double) fields from 101, so no
    // two fields share a value and a swapped pair can't cancel out.
    int v = 1;
#define SET_DIRECT(field) cfg.field = static_cast<decltype(cfg.field)>(v++);
    FOLLOW_CONFIG_DIRECT_FIELDS(SET_DIRECT)
#undef SET_DIRECT
    v = 101;
#define SET_ROUNDED(field) cfg.field = static_cast<double>(v++);
    FOLLOW_CONFIG_ROUNDED_FIELDS(SET_ROUNDED)
#undef SET_ROUNDED
    // A full-width UID on top, so an int16 narrowing bug in the record can't
    // hide behind a small sequential value.
    cfg.targetUid = 0x12345678u;
    cfg.headingMode = FOLLOW_HEADING_FIXED;

    // Guard the premise: every value really differs from the compile-time
    // default (otherwise a field the codec silently drops would still "match").
#define CHECK_NON_DEFAULT(field) \
    TEST_ASSERT_TRUE_MESSAGE(cfg.field != defaults.field, #field " test value collides with its default");
    FOLLOW_CONFIG_DIRECT_FIELDS(CHECK_NON_DEFAULT)
    FOLLOW_CONFIG_ROUNDED_FIELDS(CHECK_NON_DEFAULT)
#undef CHECK_NON_DEFAULT
    TEST_ASSERT_TRUE(cfg.headingMode != defaults.headingMode);

    const FollowRecord rec = followToRecord(cfg);
    TEST_ASSERT_EQUAL_UINT16(kFollowRecordVersion, rec.version);
    const FollowConfig back = followFromRecord(rec);

#define CHECK_CARRIED(field) \
    TEST_ASSERT_TRUE_MESSAGE(cfg.field == back.field, #field " did not survive the record round-trip");
    FOLLOW_CONFIG_DIRECT_FIELDS(CHECK_CARRIED)
    FOLLOW_CONFIG_ROUNDED_FIELDS(CHECK_CARRIED)
#undef CHECK_CARRIED
    TEST_ASSERT_EQUAL(FOLLOW_HEADING_FIXED, back.headingMode);

    // debug is RAM-only by contract: it must NOT be persisted.
    cfg.debug = true;
    TEST_ASSERT_EQUAL(defaults.debug, followFromRecord(followToRecord(cfg)).debug);
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
