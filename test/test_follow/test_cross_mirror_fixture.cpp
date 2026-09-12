// C++ side of the cross-mirror fixture. Reads the same JSON fixture the Python
// (mock server) and Node (web UI follow-logic) tests read, builds a
// FollowConfig from each case, and checks FollowController::applyConfig()'s
// verdict against that case's expectValid.

#include <unity.h>

#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "test_helpers.h"

static void setField(const JsonObjectConst& obj, const char* key, double& field) {
    if (obj.containsKey(key)) field = obj[key].as<double>();
}
static void setField(const JsonObjectConst& obj, const char* key, uint16_t& field) {
    if (obj.containsKey(key)) field = obj[key].as<uint16_t>();
}
static void setField(const JsonObjectConst& obj, const char* key, uint32_t& field) {
    if (obj.containsKey(key)) field = obj[key].as<uint32_t>();
}
static void setField(const JsonObjectConst& obj, const char* key, int16_t& field) {
    if (obj.containsKey(key)) field = obj[key].as<int16_t>();
}

static FollowConfig configFromJson(const JsonObjectConst& obj) {
    FollowConfig cfg;
    setField(obj, "ofsLongM", cfg.ofsLongM);
    setField(obj, "ofsLatM", cfg.ofsLatM);
    setField(obj, "ofsVertM", cfg.ofsVertM);
    // The fixture keeps the v1 key name "targetPeer" (shared with the Python
    // and JS validators); in v2 it is a 32-bit peer UID.
    setField(obj, "targetPeer", cfg.targetUid);
    setField(obj, "emitHz", cfg.emitHz);
    setField(obj, "peerTimeoutMs", cfg.peerTimeoutMs);
    setField(obj, "minSepM", cfg.minSepM);
    setField(obj, "minVSepM", cfg.minVSepM);
    setField(obj, "maxTargetDistM", cfg.maxTargetDistM);
    setField(obj, "minAltM", cfg.minAltM);
    setField(obj, "minCourseSpeed", cfg.minCourseSpeed);
    setField(obj, "headingDeg", cfg.headingDeg);
    setField(obj, "statusGvarIndex", cfg.statusGvarIndex);
    setField(obj, "conditionFlagsGvarIndex", cfg.conditionFlagsGvarIndex);
    setField(obj, "rcLongChannel", cfg.rcLongChannel);
    setField(obj, "rcLatChannel", cfg.rcLatChannel);
    setField(obj, "rcVertChannel", cfg.rcVertChannel);
    setField(obj, "targetSpeedGvarIndex", cfg.targetSpeedGvarIndex);
    setField(obj, "autothrottleEngageGvarIndex", cfg.autothrottleEngageGvarIndex);
    setField(obj, "autothrottleEnableRcChannel", cfg.autothrottleEnableRcChannel);
    setField(obj, "autothrottleEnableMinThresholdUs", cfg.autothrottleEnableMinThresholdUs);
    setField(obj, "autothrottleEnableMaxThresholdUs", cfg.autothrottleEnableMaxThresholdUs);
    setField(obj, "speedCorrectionAccelCmS2", cfg.speedCorrectionAccelCmS2);
    setField(obj, "minTargetSpeedMps", cfg.minTargetSpeedMps);
    setField(obj, "maxTargetSpeedMps", cfg.maxTargetSpeedMps);
    return cfg;
}

// Resolved relative to the project root -- PlatformIO's `pio test` runs
// with the project directory as the working directory.
static const char* kFixturePath = "docs/spec/fixtures/follow-config-cases.json";

static bool startsWith(const char* s, const char* prefix) {
    return s != nullptr && std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}

void test_applyConfig_matches_every_fixture_case() {
    std::ifstream file(kFixturePath);
    TEST_ASSERT_TRUE_MESSAGE(file.good(),
                             "could not open fixture -- expected CWD to be the project root");
    std::stringstream buf;
    buf << file.rdbuf();
    const std::string contents = buf.str();

    DynamicJsonDocument doc(16384);
    const DeserializationError parseErr = deserializeJson(doc, contents);
    TEST_ASSERT_FALSE_MESSAGE(parseErr, "fixture JSON failed to parse");

    JsonObjectConst baseline = doc["baseline"].as<JsonObjectConst>();
    JsonArrayConst cases = doc["cases"].as<JsonArrayConst>();
    TEST_ASSERT_TRUE(cases.size() > 0);

    FollowHarness h;
    int checked = 0;

    for (JsonObjectConst tc : cases) {
        const char* name = tc["name"].as<const char*>();
        const bool expected = tc["expectValid"].as<bool>();

        // v2 identifies the target by a 32-bit UID, so applyConfig() has no
        // targetPeer range rule any more. The v1 fixture case
        // "targetPeer_out_of_range_fails" (targetPeer: 7 -> invalid) is being
        // removed from the fixture separately; skip a targetPeer/target_peer
        // case that still expects *invalid* so this test is correct whether
        // or not that removal has landed. A targetPeer case expecting valid
        // (e.g. a large UID) is a genuine v2 rule and still runs.
        if ((startsWith(name, "targetPeer") || startsWith(name, "target_peer")) && !expected) {
            continue;
        }

        // Merge baseline + this case's overrides into one object.
        DynamicJsonDocument mergedDoc(2048);
        JsonObject merged = mergedDoc.to<JsonObject>();
        for (JsonPairConst kv : baseline) merged[kv.key()] = kv.value();
        for (JsonPairConst kv : tc["overrides"].as<JsonObjectConst>()) merged[kv.key()] = kv.value();

        const FollowConfig cfg = configFromJson(merged);
        const bool ok = h.ctl.applyConfig(cfg, nullptr);

        if (ok != expected) {
            char msg[256];
            std::snprintf(msg, sizeof(msg), "case \"%s\": expected expectValid=%s, got %s",
                          name ? name : "(unnamed)", expected ? "true" : "false",
                          ok ? "true" : "false");
            TEST_FAIL_MESSAGE(msg);
        }
        checked++;
    }
    TEST_ASSERT_TRUE_MESSAGE(checked > 0, "every fixture case was skipped");
}
