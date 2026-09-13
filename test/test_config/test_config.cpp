// The persisted configuration: JSON serialization, the partial-update merge,
// and validation.
//
// What is being pinned here is the *merge contract*, because that is the whole
// reason this module exists and it is the part that fails silently when it
// breaks. A POST carrying one key must change one key: if a future refactor
// starts rebuilding the config from the incoming document instead of merging
// onto it, every test that only checks "the field I set has the value I sent"
// still passes, while the first script that posts a single setting quietly wipes
// the WiFi credentials. So the merge tests assert on the fields that were *not*
// mentioned, and the rejection test compares the entire struct.
//
// Two more things are checked by construction rather than by example:
//
//   - the Follow block is walked with the FOLLOW_CONFIG_* X-macros, so a Follow
//     field added later is covered by the round-trip test without anyone
//     remembering to come back here;
//   - configValidate() is checked against Follow's own validator through a real
//     Follow error message, so the two cannot drift apart and hand the
//     controller a config the config layer already accepted.

// Unity's double asserts are opt-in and the whole Follow geometry is doubles.
// Defined here as well as in the build flags so this file is self-contained.
#ifndef UNITY_INCLUDE_DOUBLE
#define UNITY_INCLUDE_DOUBLE
#endif
#ifndef UNITY_DOUBLE_PRECISION
#define UNITY_DOUBLE_PRECISION 1e-9
#endif

#include <unity.h>

#include <ArduinoJson.h>

#include <cstring>
#include <string>

#include "config.h"

using namespace ff;

void setUp() {}
void tearDown() {}

// ---- Helpers -----------------------------------------------------------------

// A POST body: the JSON text a client would actually send, parsed and held so
// the JsonObjectConst handed to configMergeJson stays valid for the call.
class Body {
public:
    explicit Body(const char* json) : doc_(4096) {
        const DeserializationError e = deserializeJson(doc_, json);
        TEST_ASSERT_TRUE_MESSAGE(e == DeserializationError::Ok, json);
    }
    JsonObjectConst root() const { return doc_.as<JsonObjectConst>(); }

private:
    DynamicJsonDocument doc_;
};

// Typed field comparisons that name the field in the failure message. Overloaded
// rather than templated so each type gets Unity's own comparison (floats by
// tolerance, strings by content) and so the X-macro walk below can pass any
// Follow field in without caring what type it is.
static void expectSame(const char* what, bool a, bool b) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(a ? 1 : 0, b ? 1 : 0, what);
}
static void expectSame(const char* what, uint16_t a, uint16_t b) {
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(a, b, what);
}
static void expectSame(const char* what, uint32_t a, uint32_t b) {
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(a, b, what);
}
static void expectSame(const char* what, int16_t a, int16_t b) {
    TEST_ASSERT_EQUAL_INT16_MESSAGE(a, b, what);
}
static void expectSame(const char* what, float a, float b) {
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(a, b, what);
}
static void expectSame(const char* what, double a, double b) {
    TEST_ASSERT_EQUAL_DOUBLE_MESSAGE(a, b, what);
}
static void expectSame(const char* what, const char* a, const char* b) {
    TEST_ASSERT_EQUAL_STRING_MESSAGE(a, b, what);
}

// Every field of a FollowConfig. Driven by the X-macros so a field added to
// follow.h is compared here automatically -- the point being that a new field
// which configToJson/mergeFollow forget about fails the round-trip test on the
// day it is added, not a year later in the field.
static void assertFollowEqual(const FollowConfig& a, const FollowConfig& b) {
#define FOLLOW_CMP(field) expectSame("follow." #field, a.field, b.field);
    FOLLOW_CONFIG_DIRECT_FIELDS(FOLLOW_CMP)
    FOLLOW_CONFIG_ROUNDED_FIELDS(FOLLOW_CMP)
#undef FOLLOW_CMP
    expectSame("follow.headingMode", static_cast<int16_t>(a.headingMode),
               static_cast<int16_t>(b.headingMode));
    expectSame("follow.debug", a.debug, b.debug);
}

// Every field of a Settings. Used wherever a test needs "nothing else moved".
static void assertSettingsEqual(const Settings& a, const Settings& b) {
    expectSame("version", a.version, b.version);

    expectSame("node.name", a.node.name, b.node.name);
    expectSame("node.listen_only", a.node.listen_only, b.node.listen_only);

    expectSame("security.passphrase", a.security.passphrase, b.security.passphrase);

    expectSame("rate.target_load", a.rate.target_load, b.rate.target_load);
    expectSame("rate.min_interval_ms", a.rate.min_interval_ms, b.rate.min_interval_ms);
    expectSame("rate.max_interval_ms", a.rate.max_interval_ms, b.rate.max_interval_ms);
    expectSame("rate.jitter_frac", a.rate.jitter_frac, b.rate.jitter_frac);

    expectSame("peers.timeout_ms", a.peer_timeout_ms, b.peer_timeout_ms);
    expectSame("peers.announce_interval_ms", a.announce_interval_ms, b.announce_interval_ms);
    expectSame("msp.radar_interval_ms", a.msp_radar_interval_ms, b.msp_radar_interval_ms);
    expectSame("gnss.rate_hz", a.gnss_rate_hz, b.gnss_rate_hz);

    expectSame("radios.espnow_enabled", a.radios.espnow_enabled, b.radios.espnow_enabled);
    expectSame("radios.lora_enabled", a.radios.lora_enabled, b.radios.lora_enabled);
    expectSame("radios.lora_power_dbm", a.radios.lora_power_dbm, b.radios.lora_power_dbm);

    expectSame("wifi.ap", a.wifi.ap, b.wifi.ap);
    expectSame("wifi.ssid", a.wifi.ssid, b.wifi.ssid);
    expectSame("wifi.psk", a.wifi.psk, b.wifi.psk);
    expectSame("wifi.ap_psk", a.wifi.ap_psk, b.wifi.ap_psk);

    expectSame("sim.enabled", a.sim.enabled, b.sim.enabled);

    assertFollowEqual(a.follow, b.follow);
}

// A config with nothing left at its default, so a merge test that claims "only
// this key changed" is actually proving something: if a field were being reset
// to its compile-time default instead of carried across, this catches it.
static Settings makePopulatedConfig() {
    Settings cfg;
    std::strcpy(cfg.node.name, "chase-one");
    cfg.node.listen_only = true;
    std::strcpy(cfg.security.passphrase, "hangar-seven");

    cfg.rate.target_load = 0.22f;
    cfg.rate.min_interval_ms = 150;
    cfg.rate.max_interval_ms = 900;
    cfg.rate.jitter_frac = 0.1f;

    cfg.peer_timeout_ms = 7000;
    cfg.announce_interval_ms = 2500;
    cfg.msp_radar_interval_ms = 120;
    cfg.gnss_rate_hz = 5;

    cfg.radios.espnow_enabled = false;
    cfg.radios.lora_enabled = true;
    cfg.radios.lora_power_dbm = 17;

    cfg.wifi.ap = false;
    std::strcpy(cfg.wifi.ssid, "FormationNet");
    std::strcpy(cfg.wifi.psk, "correct-horse-battery");
    std::strcpy(cfg.wifi.ap_psk, "benchpass1");

    cfg.sim.enabled = true;

    cfg.follow.ofsLongM = -25.0;
    cfg.follow.ofsLatM = 5.0;
    cfg.follow.ofsVertM = 12.0;
    cfg.follow.targetUid = 0xDEADBEEFu;
    cfg.follow.emitHz = 5;
    cfg.follow.peerTimeoutMs = 1200;
    cfg.follow.minSepM = 9.0;
    cfg.follow.minVSepM = 14.0;
    cfg.follow.maxTargetDistM = 60.0;
    cfg.follow.minAltM = 4.0;
    cfg.follow.minCourseSpeed = 3.0;
    cfg.follow.headingMode = FOLLOW_HEADING_COURSE_RELATIVE;
    cfg.follow.headingDeg = 45.0;
    cfg.follow.statusGvarIndex = 0;
    cfg.follow.conditionFlagsGvarIndex = 1;
    cfg.follow.rcLongChannel = 5;
    cfg.follow.rcLatChannel = 6;
    cfg.follow.rcVertChannel = 7;
    cfg.follow.targetSpeedGvarIndex = 2;
    cfg.follow.autothrottleEngageGvarIndex = 3;
    cfg.follow.autothrottleEnableRcChannel = 8;
    cfg.follow.autothrottleEnableMinThresholdUs = 1600;
    cfg.follow.autothrottleEnableMaxThresholdUs = 2000;
    cfg.follow.speedCorrectionAccelCmS2 = 25;
    cfg.follow.minTargetSpeedMps = 8.0;
    cfg.follow.maxTargetSpeedMps = 30.0;
    cfg.follow.debug = true;
    return cfg;
}

// ---- Round trip ---------------------------------------------------------------

void test_defaults_are_valid() {
    const Settings cfg;
    const char* err = nullptr;
    TEST_ASSERT_TRUE(configValidate(cfg, &err));
    TEST_ASSERT_NULL(err);
}

// The GET the UI issues, posted straight back, must be a no-op.
void test_defaults_round_trip_through_json() {
    const Settings original;

    DynamicJsonDocument doc(4096);
    JsonObject o = doc.to<JsonObject>();
    configToJson(original, o);
    TEST_ASSERT_FALSE_MESSAGE(doc.overflowed(), "config does not fit the document");

    Settings restored;
    // Start from something else entirely, so "restored" cannot pass by virtue of
    // already being the defaults.
    restored = makePopulatedConfig();
    const char* err = nullptr;
    TEST_ASSERT_TRUE_MESSAGE(configMergeJson(doc.as<JsonObjectConst>(), restored, &err),
                             err == nullptr ? "" : err);
    assertSettingsEqual(original, restored);
}

void test_populated_config_round_trips_through_json() {
    const Settings original = makePopulatedConfig();
    const char* err = nullptr;
    TEST_ASSERT_TRUE_MESSAGE(configValidate(original, &err), err == nullptr ? "" : err);

    DynamicJsonDocument doc(4096);
    JsonObject o = doc.to<JsonObject>();
    configToJson(original, o);
    TEST_ASSERT_FALSE_MESSAGE(doc.overflowed(), "config does not fit the document");

    Settings restored;
    err = nullptr;
    TEST_ASSERT_TRUE_MESSAGE(configMergeJson(doc.as<JsonObjectConst>(), restored, &err),
                             err == nullptr ? "" : err);
    assertSettingsEqual(original, restored);
}

// The key names are the wire contract with the web UI (docs/v2-web-api.md).
// Renaming one breaks every saved config in the field, so pin the shape.
void test_json_has_the_documented_shape() {
    const Settings cfg;
    DynamicJsonDocument doc(4096);
    JsonObject o = doc.to<JsonObject>();
    configToJson(cfg, o);

    JsonObjectConst root = doc.as<JsonObjectConst>();
    TEST_ASSERT_TRUE(root.containsKey("version"));
    TEST_ASSERT_EQUAL_UINT16(kConfigVersion, root["version"].as<uint16_t>());
    const char* sections[] = {"node", "security", "rate",   "peers",
                              "msp",  "gnss",     "radios", "wifi",
                              "sim",  "follow"};
    for (const char* s : sections) {
        TEST_ASSERT_TRUE_MESSAGE(root.containsKey(s), s);
    }
    TEST_ASSERT_TRUE(root["security"].containsKey("passphrase"));
    TEST_ASSERT_TRUE(root["wifi"].containsKey("psk"));
    TEST_ASSERT_TRUE(root["wifi"].containsKey("ap_psk"));
    TEST_ASSERT_TRUE(root["peers"].containsKey("timeout_ms"));
    TEST_ASSERT_TRUE(root["follow"].containsKey("ofsLongM"));
    TEST_ASSERT_EQUAL_STRING("POINT_LEADER", root["follow"]["headingMode"].as<const char*>());
}

// ---- The merge contract --------------------------------------------------------

// The example straight out of docs/v2-web-api.md: one key in, one key changed.
void test_partial_post_changes_exactly_one_key() {
    Settings cfg = makePopulatedConfig();
    Settings expected = cfg;
    expected.follow.ofsLongM = -20.0;

    const Body body(R"({"follow":{"ofsLongM":-20}})");
    const char* err = nullptr;
    TEST_ASSERT_TRUE_MESSAGE(configMergeJson(body.root(), cfg, &err), err == nullptr ? "" : err);

    TEST_ASSERT_EQUAL_DOUBLE(-20.0, cfg.follow.ofsLongM);
    // Named explicitly, because these are the fields a rebuild-instead-of-merge
    // regression destroys and the ones a user notices only after a power cycle.
    TEST_ASSERT_EQUAL_STRING("FormationNet", cfg.wifi.ssid);
    TEST_ASSERT_EQUAL_STRING("correct-horse-battery", cfg.wifi.psk);
    TEST_ASSERT_EQUAL_STRING("benchpass1", cfg.wifi.ap_psk);
    TEST_ASSERT_EQUAL_STRING("hangar-seven", cfg.security.passphrase);
    TEST_ASSERT_EQUAL_STRING("chase-one", cfg.node.name);
    TEST_ASSERT_EQUAL_FLOAT(0.22f, cfg.rate.target_load);
    TEST_ASSERT_EQUAL_UINT32(7000, cfg.peer_timeout_ms);
    TEST_ASSERT_EQUAL_UINT16(5, cfg.gnss_rate_hz);
    TEST_ASSERT_EQUAL_INT16(17, cfg.radios.lora_power_dbm);
    // And the other Follow fields in the same section as the one that changed.
    TEST_ASSERT_EQUAL_DOUBLE(5.0, cfg.follow.ofsLatM);
    TEST_ASSERT_EQUAL_DOUBLE(12.0, cfg.follow.ofsVertM);
    // Everything, exhaustively.
    assertSettingsEqual(expected, cfg);
}

// The same guarantee for a key in every other section, so no section is merged
// by wholesale replacement.
void test_a_partial_post_into_each_section_leaves_the_rest_alone() {
    struct Case {
        const char* json;
        void (*expect)(Settings&);
    };
    const Case cases[] = {
        {R"({"node":{"listen_only":false}})", [](Settings& c) { c.node.listen_only = false; }},
        {R"({"security":{"passphrase":"new-group"}})",
         [](Settings& c) { std::strcpy(c.security.passphrase, "new-group"); }},
        {R"({"rate":{"jitter_frac":0.4}})", [](Settings& c) { c.rate.jitter_frac = 0.4f; }},
        {R"({"peers":{"timeout_ms":9000}})", [](Settings& c) { c.peer_timeout_ms = 9000; }},
        {R"({"msp":{"radar_interval_ms":250}})",
         [](Settings& c) { c.msp_radar_interval_ms = 250; }},
        {R"({"gnss":{"rate_hz":8}})", [](Settings& c) { c.gnss_rate_hz = 8; }},
        {R"({"radios":{"lora_power_dbm":20}})",
         [](Settings& c) { c.radios.lora_power_dbm = 20; }},
        {R"({"wifi":{"ssid":"OtherNet"}})",
         [](Settings& c) { std::strcpy(c.wifi.ssid, "OtherNet"); }},
        {R"({"sim":{"enabled":false}})", [](Settings& c) { c.sim.enabled = false; }},
        {R"({"follow":{"headingMode":"FIXED"}})",
         [](Settings& c) { c.follow.headingMode = FOLLOW_HEADING_FIXED; }},
    };

    for (const Case& tc : cases) {
        Settings cfg = makePopulatedConfig();
        Settings expected = cfg;
        tc.expect(expected);

        const Body body(tc.json);
        const char* err = nullptr;
        TEST_ASSERT_TRUE_MESSAGE(configMergeJson(body.root(), cfg, &err), tc.json);
        TEST_ASSERT_NULL_MESSAGE(err, tc.json);
        assertSettingsEqual(expected, cfg);
    }
}

void test_empty_object_is_a_validated_no_op() {
    Settings cfg = makePopulatedConfig();
    const Settings before = cfg;

    const Body body("{}");
    const char* err = nullptr;
    TEST_ASSERT_TRUE(configMergeJson(body.root(), cfg, &err));
    TEST_ASSERT_NULL(err);
    assertSettingsEqual(before, cfg);
}

// A newer UI talking to older firmware must degrade, not fail: unknown keys are
// dropped at both levels rather than rejected.
void test_unknown_keys_are_ignored() {
    Settings cfg = makePopulatedConfig();
    Settings expected = cfg;
    expected.gnss_rate_hz = 4;

    const Body body(R"({
        "future_section": {"whatever": 1},
        "version": 99,
        "gnss": {"rate_hz": 4, "constellation": "galileo"},
        "follow": {"someFieldFromNextYear": 7}
    })");
    const char* err = nullptr;
    TEST_ASSERT_TRUE_MESSAGE(configMergeJson(body.root(), cfg, &err), err == nullptr ? "" : err);
    TEST_ASSERT_NULL(err);
    assertSettingsEqual(expected, cfg);
}

// The all-or-nothing half of the contract: a body that fails validation must not
// leave the config half-applied, including the keys that were individually fine.
void test_a_rejected_merge_leaves_the_config_untouched() {
    Settings cfg = makePopulatedConfig();
    const Settings before = cfg;

    // node.name and wifi.ssid are perfectly valid; gnss.rate_hz is not.
    const Body body(R"({
        "node": {"name": "renamed", "listen_only": false},
        "wifi": {"ssid": "SomewhereElse", "psk": "hunter2hunter2"},
        "gnss": {"rate_hz": 99}
    })");
    const char* err = nullptr;
    TEST_ASSERT_FALSE(configMergeJson(body.root(), cfg, &err));
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_TRUE(err[0] != '\0');
    assertSettingsEqual(before, cfg);
}

// ---- Secret redaction ----------------------------------------------------------

void test_redaction_hides_set_secrets_and_leaves_unset_ones_empty() {
    Settings cfg;
    std::strcpy(cfg.security.passphrase, "hangar-seven");
    std::strcpy(cfg.wifi.ap_psk, "benchpass1");
    // wifi.psk is deliberately left unset.

    DynamicJsonDocument doc(4096);
    JsonObject o = doc.to<JsonObject>();
    configToJson(cfg, o, /*redact_secrets=*/true);

    JsonObjectConst root = doc.as<JsonObjectConst>();
    TEST_ASSERT_EQUAL_STRING(kRedactedSecret, root["security"]["passphrase"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING(kRedactedSecret, root["wifi"]["ap_psk"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("", root["wifi"]["psk"].as<const char*>());

    // ...and without redaction the real values come back, so the placeholder is
    // not simply what the getter always emits.
    DynamicJsonDocument clear(4096);
    JsonObject c = clear.to<JsonObject>();
    configToJson(cfg, c, /*redact_secrets=*/false);
    TEST_ASSERT_EQUAL_STRING("hangar-seven",
                             clear["security"]["passphrase"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("benchpass1", clear["wifi"]["ap_psk"].as<const char*>());
}

void test_posting_the_placeholder_back_keeps_the_stored_secret() {
    Settings cfg = makePopulatedConfig();
    const Settings before = cfg;

    DynamicJsonDocument doc(4096);
    JsonObject o = doc.to<JsonObject>();
    o.createNestedObject("security")["passphrase"] = kRedactedSecret;
    JsonObject wifi = o.createNestedObject("wifi");
    wifi["psk"] = kRedactedSecret;
    wifi["ap_psk"] = kRedactedSecret;

    const char* err = nullptr;
    TEST_ASSERT_TRUE(configMergeJson(doc.as<JsonObjectConst>(), cfg, &err));
    TEST_ASSERT_NULL(err);
    assertSettingsEqual(before, cfg);
}

// The guarantee that makes the UI work at all: fetch redacted, edit one field,
// post the whole document back, and nothing you were never shown is lost.
void test_the_redacted_document_round_trips() {
    Settings cfg = makePopulatedConfig();
    Settings expected = cfg;
    expected.node.listen_only = false;

    DynamicJsonDocument doc(4096);
    JsonObject o = doc.to<JsonObject>();
    configToJson(cfg, o, /*redact_secrets=*/true);
    o["node"]["listen_only"] = false;  // the one thing the operator changed

    const char* err = nullptr;
    TEST_ASSERT_TRUE_MESSAGE(configMergeJson(doc.as<JsonObjectConst>(), cfg, &err),
                             err == nullptr ? "" : err);
    assertSettingsEqual(expected, cfg);
}

void test_posting_a_real_secret_replaces_it() {
    Settings cfg = makePopulatedConfig();
    Settings expected = cfg;
    std::strcpy(expected.security.passphrase, "a-different-group");
    std::strcpy(expected.wifi.psk, "a-different-psk");
    std::strcpy(expected.wifi.ap_psk, "anotherbenchpass");

    const Body body(R"({
        "security": {"passphrase": "a-different-group"},
        "wifi": {"psk": "a-different-psk", "ap_psk": "anotherbenchpass"}
    })");
    const char* err = nullptr;
    TEST_ASSERT_TRUE_MESSAGE(configMergeJson(body.root(), cfg, &err), err == nullptr ? "" : err);
    assertSettingsEqual(expected, cfg);
}

// Clearing a secret has to stay possible, or an operator can never turn WPA off
// on the AP once it has been set.
void test_an_empty_string_clears_a_secret() {
    Settings cfg = makePopulatedConfig();
    const Body body(R"({"wifi":{"ap_psk":""}})");
    const char* err = nullptr;
    TEST_ASSERT_TRUE_MESSAGE(configMergeJson(body.root(), cfg, &err), err == nullptr ? "" : err);
    TEST_ASSERT_EQUAL_STRING("", cfg.wifi.ap_psk);
}

// ---- Validation ----------------------------------------------------------------

void test_validation_rules() {
    struct Case {
        const char* what;
        void (*mutate)(Settings&);
        bool expect_ok;
    };
    const Case cases[] = {
        // rate.target_load must be in (0, 1].
        {"target_load 0 (open lower bound)", [](Settings& c) { c.rate.target_load = 0.0f; },
         false},
        {"target_load negative", [](Settings& c) { c.rate.target_load = -0.1f; }, false},
        {"target_load just above 0", [](Settings& c) { c.rate.target_load = 0.001f; }, true},
        {"target_load 1.0 (closed upper bound)",
         [](Settings& c) { c.rate.target_load = 1.0f; }, true},
        {"target_load above 1.0", [](Settings& c) { c.rate.target_load = 1.001f; }, false},

        // rate.jitter_frac must be in [0, 1).
        {"jitter_frac negative", [](Settings& c) { c.rate.jitter_frac = -0.001f; }, false},
        {"jitter_frac 0 (closed lower bound)", [](Settings& c) { c.rate.jitter_frac = 0.0f; },
         true},
        {"jitter_frac just below 1", [](Settings& c) { c.rate.jitter_frac = 0.999f; }, true},
        {"jitter_frac 1.0 (open upper bound)", [](Settings& c) { c.rate.jitter_frac = 1.0f; },
         false},

        // Interval floor and ordering.
        {"min_interval_ms 0", [](Settings& c) { c.rate.min_interval_ms = 0; }, false},
        {"min_interval_ms 1", [](Settings& c) { c.rate.min_interval_ms = 1; }, true},
        {"max_interval_ms one below min",
         [](Settings& c) {
             c.rate.min_interval_ms = 500;
             c.rate.max_interval_ms = 499;
         },
         false},
        {"max_interval_ms equal to min",
         [](Settings& c) {
             c.rate.min_interval_ms = 500;
             c.rate.max_interval_ms = 500;
         },
         true},

        // Timeouts and intervals that would stall or spin a scheduler.
        {"peers.timeout_ms 0", [](Settings& c) { c.peer_timeout_ms = 0; }, false},
        {"peers.timeout_ms 1", [](Settings& c) { c.peer_timeout_ms = 1; }, true},
        {"peers.announce_interval_ms 0", [](Settings& c) { c.announce_interval_ms = 0; }, false},
        {"peers.announce_interval_ms 1", [](Settings& c) { c.announce_interval_ms = 1; }, true},
        {"msp.radar_interval_ms 0", [](Settings& c) { c.msp_radar_interval_ms = 0; }, false},
        {"msp.radar_interval_ms 1", [](Settings& c) { c.msp_radar_interval_ms = 1; }, true},

        // gnss.rate_hz must be 1-25.
        {"gnss.rate_hz 0", [](Settings& c) { c.gnss_rate_hz = 0; }, false},
        {"gnss.rate_hz 1 (lower bound)", [](Settings& c) { c.gnss_rate_hz = 1; }, true},
        {"gnss.rate_hz 25 (upper bound)", [](Settings& c) { c.gnss_rate_hz = 25; }, true},
        {"gnss.rate_hz 26", [](Settings& c) { c.gnss_rate_hz = 26; }, false},

        // At least one radio. A node that can neither hear nor speak looks
        // exactly like a hardware fault from the outside.
        {"no radio at all",
         [](Settings& c) {
             c.radios.espnow_enabled = false;
             c.radios.lora_enabled = false;
             c.sim.enabled = false;
         },
         false},
        {"espnow only",
         [](Settings& c) {
             c.radios.espnow_enabled = true;
             c.radios.lora_enabled = false;
             c.sim.enabled = false;
         },
         true},
        {"lora only",
         [](Settings& c) {
             c.radios.espnow_enabled = false;
             c.radios.lora_enabled = true;
             c.sim.enabled = false;
         },
         true},
        {"sim alone satisfies the radio rule",
         [](Settings& c) {
             c.radios.espnow_enabled = false;
             c.radios.lora_enabled = false;
             c.sim.enabled = true;
         },
         true},

        // radios.lora_power_dbm: 0 (target default) or 1-30.
        {"lora_power_dbm -1", [](Settings& c) { c.radios.lora_power_dbm = -1; }, false},
        {"lora_power_dbm 0 (target default)",
         [](Settings& c) { c.radios.lora_power_dbm = 0; }, true},
        {"lora_power_dbm 30 (upper bound)",
         [](Settings& c) { c.radios.lora_power_dbm = 30; }, true},
        {"lora_power_dbm 31", [](Settings& c) { c.radios.lora_power_dbm = 31; }, false},

        // Station mode needs somewhere to go.
        {"station mode with no ssid",
         [](Settings& c) {
             c.wifi.ap = false;
             c.wifi.ssid[0] = '\0';
         },
         false},
        {"station mode with an ssid",
         [](Settings& c) {
             c.wifi.ap = false;
             std::strcpy(c.wifi.ssid, "FormationNet");
         },
         true},
        {"ap mode needs no ssid",
         [](Settings& c) {
             c.wifi.ap = true;
             c.wifi.ssid[0] = '\0';
         },
         true},

        // WPA2's own floor; a silently-open AP is a nasty surprise.
        {"ap_psk 7 characters", [](Settings& c) { std::strcpy(c.wifi.ap_psk, "1234567"); },
         false},
        {"ap_psk 8 characters", [](Settings& c) { std::strcpy(c.wifi.ap_psk, "12345678"); },
         true},
        {"ap_psk empty (open AP)", [](Settings& c) { c.wifi.ap_psk[0] = '\0'; }, true},
    };

    for (const Case& tc : cases) {
        Settings cfg;  // defaults, which are valid
        tc.mutate(cfg);
        const char* err = nullptr;
        const bool ok = configValidate(cfg, &err);
        TEST_ASSERT_EQUAL_INT_MESSAGE(tc.expect_ok ? 1 : 0, ok ? 1 : 0, tc.what);
        // *err is set exactly when the config was rejected, never otherwise:
        // a caller that reports the message on success would print garbage.
        if (tc.expect_ok) {
            TEST_ASSERT_NULL_MESSAGE(err, tc.what);
        } else {
            TEST_ASSERT_NOT_NULL_MESSAGE(err, tc.what);
            TEST_ASSERT_TRUE_MESSAGE(err[0] != '\0', tc.what);
        }
    }
}

// configValidate with no error pointer must still reject, not crash: the
// nullptr path has its own local in the implementation.
void test_validate_without_an_error_pointer() {
    Settings cfg;
    cfg.gnss_rate_hz = 0;
    TEST_ASSERT_FALSE(configValidate(cfg));
    TEST_ASSERT_FALSE(configValidate(cfg, nullptr));

    const Body body(R"({"gnss":{"rate_hz":0}})");
    Settings live = makePopulatedConfig();
    const Settings before = live;
    TEST_ASSERT_FALSE(configMergeJson(body.root(), live));
    assertSettingsEqual(before, live);
}

// ---- Follow rules reach through --------------------------------------------------

// The config layer must enforce Follow's own rules, or a config accepted here is
// rejected later by FollowController::applyConfig and the node ends up running
// something the API said was fine.
void test_follow_rules_are_enforced_by_config_validate() {
    Settings cfg;
    // Collapse the slot below minSepM (default 8 m): a collision slot.
    cfg.follow.ofsLongM = -1.0;
    cfg.follow.ofsLatM = 0.0;
    cfg.follow.ofsVertM = 1.0;

    const char* err = nullptr;
    TEST_ASSERT_FALSE(configValidate(cfg, &err));
    TEST_ASSERT_NOT_NULL(err);
    // The message is Follow's, not a paraphrase, which is what proves the two
    // validators are the same code and cannot drift apart.
    const char* follow_err = nullptr;
    TEST_ASSERT_FALSE(followValidateConfig(cfg.follow, &follow_err));
    TEST_ASSERT_NOT_NULL(follow_err);
    TEST_ASSERT_EQUAL_STRING(follow_err, err);
    TEST_ASSERT_EQUAL_STRING("slot magnitude is below minSepM (minimum 3D separation)", err);
}

void test_an_invalid_follow_block_is_rejected_by_the_merge() {
    Settings cfg = makePopulatedConfig();
    const Settings before = cfg;

    const Body body(R"({"follow":{"ofsLongM":-1,"ofsLatM":0,"ofsVertM":1}})");
    const char* err = nullptr;
    TEST_ASSERT_FALSE(configMergeJson(body.root(), cfg, &err));
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_EQUAL_STRING("slot magnitude is below minSepM (minimum 3D separation)", err);
    assertSettingsEqual(before, cfg);

    // A second Follow rule, well away from the geometry, to show the whole
    // validator is reached and not just its last block.
    Settings other = makePopulatedConfig();
    const Body zero_hz(R"({"follow":{"emitHz":0}})");
    err = nullptr;
    TEST_ASSERT_FALSE(configMergeJson(zero_hz.root(), other, &err));
    TEST_ASSERT_EQUAL_STRING("emitHz must be > 0", err);
}

// ---- String handling -------------------------------------------------------------

void test_over_long_strings_are_truncated_not_overflowed() {
    Settings cfg;

    const std::string long_name(kMaxNameLen + 40, 'n');
    const std::string long_ssid(kMaxSsidLen + 40, 's');
    const std::string long_pass(kMaxPassphraseLen + 40, 'p');
    const std::string long_psk(kMaxPskLen + 40, 'k');

    DynamicJsonDocument doc(4096);
    JsonObject o = doc.to<JsonObject>();
    o.createNestedObject("node")["name"] = long_name;
    o.createNestedObject("security")["passphrase"] = long_pass;
    JsonObject wifi = o.createNestedObject("wifi");
    wifi["ssid"] = long_ssid;
    wifi["psk"] = long_psk;
    wifi["ap_psk"] = long_psk;

    const char* err = nullptr;
    TEST_ASSERT_TRUE_MESSAGE(configMergeJson(doc.as<JsonObjectConst>(), cfg, &err),
                             err == nullptr ? "" : err);

    TEST_ASSERT_EQUAL_size_t(kMaxNameLen, std::strlen(cfg.node.name));
    TEST_ASSERT_EQUAL_CHAR('\0', cfg.node.name[kMaxNameLen]);
    TEST_ASSERT_EQUAL_size_t(kMaxPassphraseLen, std::strlen(cfg.security.passphrase));
    TEST_ASSERT_EQUAL_CHAR('\0', cfg.security.passphrase[kMaxPassphraseLen]);
    TEST_ASSERT_EQUAL_size_t(kMaxSsidLen, std::strlen(cfg.wifi.ssid));
    TEST_ASSERT_EQUAL_CHAR('\0', cfg.wifi.ssid[kMaxSsidLen]);
    TEST_ASSERT_EQUAL_size_t(kMaxPskLen, std::strlen(cfg.wifi.psk));
    TEST_ASSERT_EQUAL_CHAR('\0', cfg.wifi.psk[kMaxPskLen]);
    TEST_ASSERT_EQUAL_size_t(kMaxPskLen, std::strlen(cfg.wifi.ap_psk));
    TEST_ASSERT_EQUAL_CHAR('\0', cfg.wifi.ap_psk[kMaxPskLen]);

    // And the truncated value is the leading bytes of what was sent, not some
    // shifted window of it.
    TEST_ASSERT_EQUAL_STRING_LEN(long_name.c_str(), cfg.node.name, kMaxNameLen);
    TEST_ASSERT_EQUAL_STRING_LEN(long_ssid.c_str(), cfg.wifi.ssid, kMaxSsidLen);

    // Truncated strings must survive a trip back out and in again unchanged,
    // rather than being truncated a second time or re-lengthened.
    const Settings after_truncation = cfg;
    DynamicJsonDocument out(4096);
    JsonObject oo = out.to<JsonObject>();
    configToJson(cfg, oo);
    Settings restored;
    err = nullptr;
    TEST_ASSERT_TRUE(configMergeJson(out.as<JsonObjectConst>(), restored, &err));
    assertSettingsEqual(after_truncation, restored);
}

// A value of the wrong JSON type must not corrupt the field: copyStr only acts
// on strings, so a number posted into a string field leaves it alone.
void test_a_non_string_value_leaves_a_string_field_alone() {
    Settings cfg = makePopulatedConfig();
    const Settings before = cfg;

    const Body body(R"({"node":{"name":12345},"wifi":{"ssid":null}})");
    const char* err = nullptr;
    TEST_ASSERT_TRUE_MESSAGE(configMergeJson(body.root(), cfg, &err), err == nullptr ? "" : err);
    assertSettingsEqual(before, cfg);
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_defaults_are_valid);
    RUN_TEST(test_defaults_round_trip_through_json);
    RUN_TEST(test_populated_config_round_trips_through_json);
    RUN_TEST(test_json_has_the_documented_shape);

    RUN_TEST(test_partial_post_changes_exactly_one_key);
    RUN_TEST(test_a_partial_post_into_each_section_leaves_the_rest_alone);
    RUN_TEST(test_empty_object_is_a_validated_no_op);
    RUN_TEST(test_unknown_keys_are_ignored);
    RUN_TEST(test_a_rejected_merge_leaves_the_config_untouched);

    RUN_TEST(test_redaction_hides_set_secrets_and_leaves_unset_ones_empty);
    RUN_TEST(test_posting_the_placeholder_back_keeps_the_stored_secret);
    RUN_TEST(test_the_redacted_document_round_trips);
    RUN_TEST(test_posting_a_real_secret_replaces_it);
    RUN_TEST(test_an_empty_string_clears_a_secret);

    RUN_TEST(test_validation_rules);
    RUN_TEST(test_validate_without_an_error_pointer);

    RUN_TEST(test_follow_rules_are_enforced_by_config_validate);
    RUN_TEST(test_an_invalid_follow_block_is_rejected_by_the_merge);

    RUN_TEST(test_over_long_strings_are_truncated_not_overflowed);
    RUN_TEST(test_a_non_string_value_leaves_a_string_field_alone);

    return UNITY_END();
}
