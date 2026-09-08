// C++ side of the cross-mirror fixture. Reads the same JSON fixture the Python
// (test_mock_server.py) and Node (test/follow-logic.test.js) tests read,
// builds a FollowRuntimeConfig from each case, and checks
// FollowManager::applyConfig()'s verdict against that case's expectValid.

#include <unity.h>
#include <fstream>
#include <sstream>

#include "test_helpers.h"

static void setField(const JsonObjectConst &obj, const char *key, double &field)
{
    if (obj.containsKey(key)) field = obj[key].as<double>();
}
static void setField(const JsonObjectConst &obj, const char *key, uint8_t &field)
{
    if (obj.containsKey(key)) field = obj[key].as<uint8_t>();
}
static void setField(const JsonObjectConst &obj, const char *key, uint16_t &field)
{
    if (obj.containsKey(key)) field = obj[key].as<uint16_t>();
}
static void setField(const JsonObjectConst &obj, const char *key, uint32_t &field)
{
    if (obj.containsKey(key)) field = obj[key].as<uint32_t>();
}
static void setField(const JsonObjectConst &obj, const char *key, int16_t &field)
{
    if (obj.containsKey(key)) field = obj[key].as<int16_t>();
}

static FollowRuntimeConfig configFromJson(const JsonObjectConst &obj)
{
    FollowRuntimeConfig cfg;
    setField(obj, "ofsLongM", cfg.ofsLongM);
    setField(obj, "ofsLatM", cfg.ofsLatM);
    setField(obj, "ofsVertM", cfg.ofsVertM);
    setField(obj, "targetPeer", cfg.targetPeer);
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
static const char *kFixturePath = "docs/spec/fixtures/follow-config-cases.json";

void test_applyConfig_matches_every_fixture_case()
{
    std::ifstream file(kFixturePath);
    TEST_ASSERT_TRUE_MESSAGE(file.good(), "could not open fixture -- expected CWD to be the project root");
    std::stringstream buf;
    buf << file.rdbuf();
    std::string contents = buf.str();

    DynamicJsonDocument doc(16384);
    DeserializationError parseErr = deserializeJson(doc, contents);
    TEST_ASSERT_FALSE_MESSAGE(parseErr, "fixture JSON failed to parse");

    JsonObjectConst baseline = doc["baseline"].as<JsonObjectConst>();
    JsonArrayConst cases = doc["cases"].as<JsonArrayConst>();
    TEST_ASSERT_TRUE(cases.size() > 0);

    FakeMsp msp;
    FakeGnss gnss;
    FakePeers peers;
    FollowManager fm(&msp, &gnss, &peers);

    for (JsonObjectConst tc : cases)
    {
        // Merge baseline + this case's overrides into one object.
        DynamicJsonDocument mergedDoc(2048);
        JsonObject merged = mergedDoc.to<JsonObject>();
        for (JsonPairConst kv : baseline) merged[kv.key()] = kv.value();
        for (JsonPairConst kv : tc["overrides"].as<JsonObjectConst>()) merged[kv.key()] = kv.value();

        FollowRuntimeConfig cfg = configFromJson(merged);
        String err;
        bool ok = fm.applyConfig(cfg, &err);
        bool expected = tc["expectValid"].as<bool>();

        if (ok != expected)
        {
            String msg = String("case \"") + tc["name"].as<const char *>() + "\": expected expectValid=" +
                         (expected ? "true" : "false") + ", got " + (ok ? "true" : "false");
            TEST_FAIL_MESSAGE(msg.c_str());
        }
    }
}
