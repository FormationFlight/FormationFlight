// applyConfig() validation + EEPROM round-trip, plus configJson()/
// statusJson()'s REST contract shape.

#include <unity.h>
#include <cstring>

#include "test_helpers.h"
#include "main.h" // for `cfg` -- FOLLOW_EEPROM_OFFSET == sizeof(cfg)
#include "EEPROM.h"

// ---- §4.13 applyConfig() validation: every rejection rule, boundary-fail
// and boundary-pass, table-driven. ----

struct ConfigCase {
    const char *name;
    void (*mutate)(FollowRuntimeConfig &);
    bool expectValid;
};

// clang-format off
static const ConfigCase kConfigCases[] = {
    {"emitHz=0 fails",                 [](FollowRuntimeConfig &c) { c.emitHz = 0; },                 false},
    {"emitHz=1 passes",                [](FollowRuntimeConfig &c) { c.emitHz = 1; },                 true},
    {"peerTimeoutMs=0 fails",          [](FollowRuntimeConfig &c) { c.peerTimeoutMs = 0; },          false},
    {"peerTimeoutMs=1 passes",         [](FollowRuntimeConfig &c) { c.peerTimeoutMs = 1; },          true},
    {"minSepM=-1 fails",               [](FollowRuntimeConfig &c) { c.minSepM = -1; },               false},
    {"minVSepM=-1 fails",              [](FollowRuntimeConfig &c) { c.minVSepM = -1; },              false},
    {"minAltM=-1 fails",               [](FollowRuntimeConfig &c) { c.minAltM = -1; },               false},
    {"minSepM/minVSepM/minAltM=0 passes", [](FollowRuntimeConfig &c) { c.minSepM = 0; c.minVSepM = 0; c.minAltM = 0; }, true},
    {"maxTargetDistM=0 fails",         [](FollowRuntimeConfig &c) { c.maxTargetDistM = 0; },         false},
    {"maxTargetDistM=0.001 passes",    [](FollowRuntimeConfig &c) { c.maxTargetDistM = 0.001; },     true},
    {"minCourseSpeed=-1 fails",        [](FollowRuntimeConfig &c) { c.minCourseSpeed = -1; },        false},
    {"minCourseSpeed=0 passes",        [](FollowRuntimeConfig &c) { c.minCourseSpeed = 0; },         true},
    {"targetPeer=NODES_MAX+1 fails",   [](FollowRuntimeConfig &c) { c.targetPeer = NODES_MAX + 1; }, false},
    {"targetPeer=NODES_MAX passes",    [](FollowRuntimeConfig &c) { c.targetPeer = NODES_MAX; },     true},
    {"statusGvarIndex=-2 fails",       [](FollowRuntimeConfig &c) { c.statusGvarIndex = -2; },       false},
    {"statusGvarIndex=8 fails",        [](FollowRuntimeConfig &c) { c.statusGvarIndex = 8; },        false},
    {"statusGvarIndex=-1 passes",      [](FollowRuntimeConfig &c) { c.statusGvarIndex = -1; },       true},
    {"statusGvarIndex=7 passes",       [](FollowRuntimeConfig &c) { c.statusGvarIndex = 7; },        true},
    {"conditionFlagsGvarIndex=-2 fails", [](FollowRuntimeConfig &c) { c.conditionFlagsGvarIndex = -2; }, false},
    {"conditionFlagsGvarIndex=8 fails", [](FollowRuntimeConfig &c) { c.conditionFlagsGvarIndex = 8; }, false},
    {"conditionFlagsGvarIndex=7 passes", [](FollowRuntimeConfig &c) { c.conditionFlagsGvarIndex = 7; }, true},
    {"rcLongChannel=0 fails",          [](FollowRuntimeConfig &c) { c.rcLongChannel = 0; },          false},
    {"rcLongChannel=17 fails",         [](FollowRuntimeConfig &c) { c.rcLongChannel = 17; },         false},
    {"rcLongChannel=1 passes",         [](FollowRuntimeConfig &c) { c.rcLongChannel = 1; },          true},
    {"rcLongChannel=16 passes",        [](FollowRuntimeConfig &c) { c.rcLongChannel = 16; },         true},
    {"rcLatChannel=0 fails",           [](FollowRuntimeConfig &c) { c.rcLatChannel = 0; },           false},
    {"rcLatChannel=16 passes",         [](FollowRuntimeConfig &c) { c.rcLatChannel = 16; },          true},
    {"rcVertChannel=0 fails",          [](FollowRuntimeConfig &c) { c.rcVertChannel = 0; },          false},
    {"rcVertChannel=16 passes",        [](FollowRuntimeConfig &c) { c.rcVertChannel = 16; },         true},
    {"targetSpeedGvarIndex=8 fails",   [](FollowRuntimeConfig &c) { c.targetSpeedGvarIndex = 8; },   false},
    {"targetSpeedGvarIndex=7 passes",  [](FollowRuntimeConfig &c) { c.targetSpeedGvarIndex = 7; },   true},
    {"autothrottleEngageGvarIndex=8 fails", [](FollowRuntimeConfig &c) { c.autothrottleEngageGvarIndex = 8; }, false},
    {"autothrottleEngageGvarIndex=7 passes", [](FollowRuntimeConfig &c) { c.autothrottleEngageGvarIndex = 7; }, true},
    {"autothrottleEnableRcChannel=0 fails", [](FollowRuntimeConfig &c) { c.autothrottleEnableRcChannel = 0; }, false},
    {"autothrottleEnableRcChannel=16 passes", [](FollowRuntimeConfig &c) { c.autothrottleEnableRcChannel = 16; c.minTargetSpeedMps = 5; c.maxTargetSpeedMps = 30; }, true},
    {"disarmed with default (0/0) target speeds passes", [](FollowRuntimeConfig &c) { (void)c; }, true},
    {"armed with default (0/0) target speeds fails", [](FollowRuntimeConfig &c) { c.autothrottleEnableRcChannel = 6; }, false},
    {"armed, minTargetSpeedMps=0 fails", [](FollowRuntimeConfig &c) { c.autothrottleEnableRcChannel = 6; c.minTargetSpeedMps = 0; c.maxTargetSpeedMps = 10; }, false},
    {"armed, minTargetSpeedMps=-1 fails", [](FollowRuntimeConfig &c) { c.autothrottleEnableRcChannel = 6; c.minTargetSpeedMps = -1; c.maxTargetSpeedMps = 10; }, false},
    {"armed, maxTargetSpeedMps==minTargetSpeedMps fails", [](FollowRuntimeConfig &c) { c.autothrottleEnableRcChannel = 6; c.minTargetSpeedMps = 10; c.maxTargetSpeedMps = 10; }, false},
    {"armed, maxTargetSpeedMps>minTargetSpeedMps>0 passes", [](FollowRuntimeConfig &c) { c.autothrottleEnableRcChannel = 6; c.minTargetSpeedMps = 10; c.maxTargetSpeedMps = 10.1; }, true},
    {"speedCorrectionAccelCmS2=-1 fails", [](FollowRuntimeConfig &c) { c.speedCorrectionAccelCmS2 = -1; }, false},
    {"speedCorrectionAccelCmS2=0 passes", [](FollowRuntimeConfig &c) { c.speedCorrectionAccelCmS2 = 0; }, true},
    {"offset magnitude just under minSepM fails", [](FollowRuntimeConfig &c) { c.ofsLongM = -2; c.ofsLatM = 0; c.ofsVertM = 0; c.minSepM = 8; }, false},
    {"offset magnitude at minSepM passes", [](FollowRuntimeConfig &c) { c.ofsLongM = -8; c.ofsLatM = 0; c.ofsVertM = 0; c.minSepM = 8; }, true},
    // These overlap/ordering checks are also enforced client-side in
    // html/follow-logic.js's validateConfig(), so a raw POST bypassing the
    // web UI still gets the same guarantee.
    {"statusGvarIndex==conditionFlagsGvarIndex fails", [](FollowRuntimeConfig &c) { c.statusGvarIndex = 1; c.conditionFlagsGvarIndex = 1; }, false},
    {"rcLongChannel==rcLatChannel fails", [](FollowRuntimeConfig &c) { c.rcLongChannel = 5; c.rcLatChannel = 5; }, false},
    {"autothrottleEnableRcChannel overlaps rc axis channel fails", [](FollowRuntimeConfig &c) { c.rcLongChannel = 5; c.autothrottleEnableRcChannel = 5; }, false},
    {"autothrottleEnableMaxThresholdUs<=MinThresholdUs fails", [](FollowRuntimeConfig &c) { c.autothrottleEnableMinThresholdUs = 2100; c.autothrottleEnableMaxThresholdUs = 1700; }, false},
};
// clang-format on

void test_applyConfig_validation_rules_table()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);

    for (const auto &tc : kConfigCases)
    {
        // Default-constructed, not fm.getConfig() -- the latter reflects
        // whatever the last *accepted* case in this loop left live, so an
        // earlier passing case (e.g. statusGvarIndex=7) would otherwise
        // leak into a later one (e.g. conditionFlagsGvarIndex=7) and the
        // GVAR-uniqueness rule would reject it for a reason unrelated to
        // what this case is actually testing.
        FollowRuntimeConfig cfg;
        tc.mutate(cfg);
        String err;
        bool ok = fm.applyConfig(cfg, &err);
        if (ok != tc.expectValid)
        {
            String msg = String(tc.name) + (tc.expectValid ? " expected valid" : " expected invalid");
            TEST_FAIL_MESSAGE(msg.c_str());
        }
    }
}

void test_rejected_applyConfig_leaves_live_config_untouched()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);

    FollowRuntimeConfig before = fm.getConfig();

    FollowRuntimeConfig bad = before;
    bad.emitHz = 0; // guaranteed rejection
    bad.ofsLongM = 12345.0; // a field that would be obviously different if it leaked through
    String err;
    TEST_ASSERT_FALSE(fm.applyConfig(bad, &err));

    FollowRuntimeConfig after = fm.getConfig();
    TEST_ASSERT_EQUAL_DOUBLE(before.ofsLongM, after.ofsLongM);
    TEST_ASSERT_EQUAL(before.emitHz, after.emitHz);
}

// ---- EEPROM round-trip (via the public saveToEEPROM()/loadFromEEPROM(),
// which internally exercise toEepromRecord()/fromEepromRecord()) ----

void test_eeprom_round_trip_preserves_fields_with_documented_rounding()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);

    FollowRuntimeConfig cfg = fm.getConfig();
    cfg.ofsLongM = 15.6;         // lround -> 16
    cfg.ofsLatM = -15.6;         // lround -> -16 (away from zero)
    cfg.minSepM = 3.4;           // lround -> 3
    cfg.minVSepM = 0;
    cfg.headingDeg = 99.5;       // lround -> 100
    cfg.minTargetSpeedMps = 5.5; // lround -> 6
    cfg.maxTargetSpeedMps = 30.5; // lround -> 31 (still > minTargetSpeedMps after rounding, irrelevant to the raw double stored pre-round anyway)
    String err;
    TEST_ASSERT_TRUE(fm.applyConfig(cfg, &err));

    String saveErr;
    TEST_ASSERT_TRUE(fm.saveToEEPROM(&saveErr));

    FollowManager fm2(&msp, &gnss, &peers); // fresh instance, compile-time defaults
    fm2.loadFromEEPROM();
    FollowRuntimeConfig loaded = fm2.getConfig();

    TEST_ASSERT_EQUAL_DOUBLE(16.0, loaded.ofsLongM);
    TEST_ASSERT_EQUAL_DOUBLE(-16.0, loaded.ofsLatM);
    TEST_ASSERT_EQUAL_DOUBLE(3.0, loaded.minSepM);
    TEST_ASSERT_EQUAL_DOUBLE(100.0, loaded.headingDeg);
    TEST_ASSERT_EQUAL_DOUBLE(6.0, loaded.minTargetSpeedMps);
    TEST_ASSERT_EQUAL_DOUBLE(31.0, loaded.maxTargetSpeedMps);
    // Non-fractional integer fields carry through unchanged.
    TEST_ASSERT_EQUAL(cfg.targetPeer, loaded.targetPeer);
    TEST_ASSERT_EQUAL(cfg.emitHz, loaded.emitHz);
}

void test_eeprom_version_mismatch_falls_back_to_defaults()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;

    // Corrupt/uninitialized EEPROM: a record whose version doesn't match
    // what loadFromEEPROM() expects. FOLLOW_EEPROM_OFFSET == sizeof(cfg) --
    // the same stub `cfg` global this test binary and FollowManager.cpp
    // both link against, so this lands at the exact offset loadFromEEPROM()
    // reads from.
    FollowEepromRecord bad{};
    bad.version = 0; // never a real FOLLOW_EEPROM_VERSION value
    EEPROM.put((int)sizeof(cfg), bad);

    FollowManager fm(&msp, &gnss, &peers);
    fm.loadFromEEPROM();

    // Compile-time default untouched, not a crash or a partially-applied
    // garbage record.
    TEST_ASSERT_EQUAL_DOUBLE(-15.0, fm.getConfig().ofsLongM);
}

void test_eeprom_save_rate_limited_second_call_fails_first_persists()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);

    FollowRuntimeConfig cfg1 = fm.getConfig();
    cfg1.ofsLongM = -8.0; // still geometry-sane against default minSepM(8)
    String err;
    TEST_ASSERT_TRUE(fm.applyConfig(cfg1, &err));
    String saveErr1;
    TEST_ASSERT_TRUE(fm.saveToEEPROM(&saveErr1)); // first save: no prior commit, always allowed

    FollowRuntimeConfig cfg2 = fm.getConfig();
    cfg2.ofsLongM = -9.0;
    TEST_ASSERT_TRUE(fm.applyConfig(cfg2, &err));
    String saveErr2;
    TEST_ASSERT_FALSE(fm.saveToEEPROM(&saveErr2)); // same instant -> within FOLLOW_EEPROM_COMMIT_MIN_INTERVAL_MS

    FollowManager fm2(&msp, &gnss, &peers);
    fm2.loadFromEEPROM();
    TEST_ASSERT_EQUAL_DOUBLE(-8.0, fm2.getConfig().ofsLongM); // first save's data, not the second (rejected) one
}

// ---- §4.14 REST contract shape ----

void test_configJson_emits_every_documented_field()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);

    DynamicJsonDocument doc(2048);
    fm.configJson(&doc);

    // Matches docs/user-guide-follow-mode.md §11's GET /followmanager/config table.
    static const char *kFields[] = {
        "ofsLongM", "ofsLatM", "ofsVertM", "triggerMode", "targetPeer", "emitHz",
        "peerTimeoutMs", "minSepM", "minVSepM", "maxTargetDistM", "minAltM",
        "minCourseSpeed", "headingMode", "headingDeg", "statusGvarIndex",
        "conditionFlagsGvarIndex", "rcLongChannel", "rcLatChannel", "rcVertChannel",
        "targetSpeedGvarIndex", "autothrottleEngageGvarIndex", "autothrottleEnableRcChannel",
        "autothrottleEnableMinThresholdUs", "autothrottleEnableMaxThresholdUs",
        "speedCorrectionAccelCmS2", "minTargetSpeedMps", "maxTargetSpeedMps", "debug",
    };
    for (const char *field : kFields)
    {
        if (!doc.containsKey(field))
        {
            String msg = String("configJson() missing field: ") + field;
            TEST_FAIL_MESSAGE(msg.c_str());
        }
    }
}

void test_statusJson_conditional_fields_present_and_absent_as_documented()
{
    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers; // no peer -> haveLastTarget/havePreArmCandidateOffset stay false
    FollowManager fm(&msp, &gnss, &peers);
    msp.gcsNavActive = false;

    followTick(fm);
    DynamicJsonDocument absentDoc(1024);
    fm.statusJson(&absentDoc);
    TEST_ASSERT_FALSE(absentDoc.containsKey("lastTarget"));
    TEST_ASSERT_FALSE(absentDoc.containsKey("liveOffset"));
    TEST_ASSERT_FALSE(absentDoc.containsKey("preArmCandidateOffset"));
    TEST_ASSERT_FALSE(absentDoc.containsKey("statusGvarValue")); // index disabled (-1)

    // Now drive to a fully-locked, target-emitted, pre-arm-computed state.
    FollowRuntimeConfig cfg = fm.getConfig();
    cfg.rcLongChannel = 5; // so havePreArmCandidateOffset can go true while disarmed
    cfg.statusGvarIndex = 1;
    String err;
    TEST_ASSERT_TRUE(fm.applyConfig(cfg, &err));
    msp.state = 0; // disarmed -> pre-arm candidate computed
    msp.gcsNavActive = true;
    peers.setPeer(0, /*id=*/1, 37.0, -122.0, 10.0, 0.0);
    GNSSLocation self{};
    self.lat = 37.0;
    self.lon = -122.0;
    gnss.setSelf(self);

    followTick(fm);
    DynamicJsonDocument presentDoc(1024);
    fm.statusJson(&presentDoc);
    TEST_ASSERT_TRUE(presentDoc.containsKey("lastTarget"));
    TEST_ASSERT_TRUE(presentDoc.containsKey("liveOffset"));
    TEST_ASSERT_TRUE(presentDoc.containsKey("preArmCandidateOffset"));
    TEST_ASSERT_TRUE(presentDoc.containsKey("statusGvarValue"));
}
