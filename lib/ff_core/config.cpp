#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ff {

namespace {

// Copies a JSON string into a fixed buffer, truncating rather than overflowing.
// The redaction placeholder is treated as "unchanged" so a UI can post back the
// document it was given without blanking the secrets it was never shown.
void copyStr(JsonVariantConst v, char* dst, size_t cap) {
    if (!v.is<const char*>()) {
        return;
    }
    const char* s = v.as<const char*>();
    if (s == nullptr) {
        return;
    }
    if (std::strcmp(s, kRedactedSecret) == 0) {
        return;
    }
    std::strncpy(dst, s, cap);
    dst[cap] = '\0';
}

void putSecret(JsonObject obj, const char* key, const char* value, bool redact) {
    if (redact && value[0] != '\0') {
        obj[key] = kRedactedSecret;
    } else {
        obj[key] = value;
    }
}

// Reads a key only if present, leaving the destination alone otherwise. This is
// the whole merge contract in one helper.
template <typename T>
void mergeVal(JsonObjectConst in, const char* key, T& dst) {
    if (in.containsKey(key)) {
        dst = in[key].as<T>();
    }
}

void mergeFollow(JsonObjectConst in, FollowConfig& f) {
#define MERGE_DIRECT(field) mergeVal(in, #field, f.field);
    FOLLOW_CONFIG_DIRECT_FIELDS(MERGE_DIRECT)
#undef MERGE_DIRECT
#define MERGE_ROUNDED(field) mergeVal(in, #field, f.field);
    FOLLOW_CONFIG_ROUNDED_FIELDS(MERGE_ROUNDED)
#undef MERGE_ROUNDED
    // targetUid travels as an 8-char hex string like every other UID in the API,
    // so it has to be re-read after the macro above (which took it as a number
    // and got 0). A bare number is still accepted, because a config file written
    // by hand is easier to get right that way than to reject over a quote.
    if (in.containsKey("targetUid")) {
        JsonVariantConst v = in["targetUid"];
        if (v.is<const char*>() && v.as<const char*>() != nullptr) {
            f.targetUid = static_cast<uint32_t>(std::strtoul(v.as<const char*>(), nullptr, 16));
        } else {
            f.targetUid = v.as<uint32_t>();
        }
    }
    mergeVal(in, "debug", f.debug);
    if (in.containsKey("headingMode")) {
        const char* m = in["headingMode"].as<const char*>();
        if (m != nullptr) {
            if (std::strcmp(m, "OFF") == 0) {
                f.headingMode = FOLLOW_HEADING_OFF;
            } else if (std::strcmp(m, "COURSE") == 0) {
                f.headingMode = FOLLOW_HEADING_COURSE;
            } else if (std::strcmp(m, "POINT_LEADER") == 0) {
                f.headingMode = FOLLOW_HEADING_POINT_LEADER;
            } else if (std::strcmp(m, "FIXED") == 0) {
                f.headingMode = FOLLOW_HEADING_FIXED;
            } else if (std::strcmp(m, "COURSE_RELATIVE") == 0) {
                f.headingMode = FOLLOW_HEADING_COURSE_RELATIVE;
            }
        }
    }
}

void followToJson(const FollowConfig& f, JsonObject out) {
#define JSON_FIELD(field) out[#field] = f.field;
    FOLLOW_CONFIG_DIRECT_FIELDS(JSON_FIELD)
    FOLLOW_CONFIG_ROUNDED_FIELDS(JSON_FIELD)
#undef JSON_FIELD
    // Overwrite the number the macro just wrote with the hex form. ArduinoJson
    // copies a non-const char* rather than storing the pointer, so the local
    // buffer going out of scope is safe (and test_config's round trip proves it).
    char target_uid[9];
    std::snprintf(target_uid, sizeof(target_uid), "%08x", static_cast<unsigned>(f.targetUid));
    out["targetUid"] = target_uid;
    out["headingMode"] = followHeadingModeName(f.headingMode);
    out["triggerMode"] = followTriggerModeName(static_cast<FollowTriggerMode>(FOLLOW_TRIGGER_MODE));
    out["debug"] = f.debug;
}

}  // namespace

void configToJson(const Settings& cfg, JsonObject out, bool redact_secrets) {
    out["version"] = cfg.version;

    JsonObject node = out.createNestedObject("node");
    node["name"] = cfg.node.name;
    node["listen_only"] = cfg.node.listen_only;

    JsonObject sec = out.createNestedObject("security");
    putSecret(sec, "passphrase", cfg.security.passphrase, redact_secrets);

    JsonObject rate = out.createNestedObject("rate");
    rate["target_load"] = cfg.rate.target_load;
    rate["min_interval_ms"] = cfg.rate.min_interval_ms;
    rate["max_interval_ms"] = cfg.rate.max_interval_ms;
    rate["jitter_frac"] = cfg.rate.jitter_frac;
    rate["duty_cycle_pct"] = cfg.rate.duty_cycle_pct;

    JsonObject peers = out.createNestedObject("peers");
    peers["timeout_ms"] = cfg.peer_timeout_ms;
    peers["announce_interval_ms"] = cfg.announce_interval_ms;

    JsonObject msp = out.createNestedObject("msp");
    msp["radar_interval_ms"] = cfg.msp_radar_interval_ms;

    JsonObject gnss = out.createNestedObject("gnss");
    gnss["rate_hz"] = cfg.gnss_rate_hz;

    JsonObject radios = out.createNestedObject("radios");
    radios["espnow_enabled"] = cfg.radios.espnow_enabled;
    radios["lora_enabled"] = cfg.radios.lora_enabled;
    radios["lora_power_dbm"] = cfg.radios.lora_power_dbm;

    JsonObject wifi = out.createNestedObject("wifi");
    wifi["ap"] = cfg.wifi.ap;
    wifi["ssid"] = cfg.wifi.ssid;
    putSecret(wifi, "psk", cfg.wifi.psk, redact_secrets);
    putSecret(wifi, "ap_psk", cfg.wifi.ap_psk, redact_secrets);
    wifi["channel"] = cfg.wifi.channel;

    JsonObject sim = out.createNestedObject("sim");
    sim["enabled"] = cfg.sim.enabled;

    followToJson(cfg.follow, out.createNestedObject("follow"));
}

bool configValidate(const Settings& cfg, const char** err) {
    const char* local = nullptr;
    if (err == nullptr) {
        err = &local;
    }

    if (cfg.rate.target_load <= 0.0f || cfg.rate.target_load > 1.0f) {
        *err = "rate.target_load must be in (0, 1]";
        return false;
    }
    if (cfg.rate.jitter_frac < 0.0f || cfg.rate.jitter_frac >= 1.0f) {
        *err = "rate.jitter_frac must be in [0, 1)";
        return false;
    }
    if (cfg.rate.min_interval_ms == 0) {
        *err = "rate.min_interval_ms must be > 0";
        return false;
    }
    if (cfg.rate.max_interval_ms < cfg.rate.min_interval_ms) {
        *err = "rate.max_interval_ms must be >= rate.min_interval_ms";
        return false;
    }
    if (cfg.rate.duty_cycle_pct > 100) {
        *err = "rate.duty_cycle_pct must be 0 (no limit) or 1-100";
        return false;
    }
    if (cfg.peer_timeout_ms == 0) {
        *err = "peers.timeout_ms must be > 0";
        return false;
    }
    if (cfg.announce_interval_ms == 0) {
        *err = "peers.announce_interval_ms must be > 0";
        return false;
    }
    if (cfg.msp_radar_interval_ms == 0) {
        *err = "msp.radar_interval_ms must be > 0";
        return false;
    }
    if (cfg.gnss_rate_hz == 0 || cfg.gnss_rate_hz > 25) {
        *err = "gnss.rate_hz must be 1-25";
        return false;
    }
    // A node with no radio enabled is almost certainly a mistake, and it looks
    // identical to a hardware fault from the outside. Refuse it.
    if (!cfg.radios.espnow_enabled && !cfg.radios.lora_enabled && !cfg.sim.enabled) {
        *err = "at least one radio must be enabled";
        return false;
    }
    if (cfg.radios.lora_power_dbm < 0 || cfg.radios.lora_power_dbm > 30) {
        *err = "radios.lora_power_dbm must be 0 (target default) or 1-30";
        return false;
    }
    if (cfg.wifi.channel < 1 || cfg.wifi.channel > 13) {
        *err = "wifi.channel must be 1-13";
        return false;
    }
    if (!cfg.wifi.ap && cfg.wifi.ssid[0] == '\0') {
        *err = "wifi.ssid is required when wifi.ap is false";
        return false;
    }
    // WPA2 will not accept a shorter key, and an AP that silently comes up open
    // because the password was too short is a nasty surprise.
    if (cfg.wifi.ap_psk[0] != '\0' && std::strlen(cfg.wifi.ap_psk) < 8) {
        *err = "wifi.ap_psk must be empty (open) or at least 8 characters";
        return false;
    }

    // Follow's own rules, so a config accepted here can never be rejected by the
    // controller later.
    if (!followValidateConfig(cfg.follow, err)) {
        return false;
    }
    return true;
}

bool configMergeJson(JsonObjectConst in, Settings& cfg, const char** err) {
    Settings next = cfg;

    if (in.containsKey("node")) {
        JsonObjectConst node = in["node"];
        copyStr(node["name"], next.node.name, kMaxNameLen);
        mergeVal(node, "listen_only", next.node.listen_only);
    }
    if (in.containsKey("security")) {
        JsonObjectConst sec = in["security"];
        copyStr(sec["passphrase"], next.security.passphrase, kMaxPassphraseLen);
    }
    if (in.containsKey("rate")) {
        JsonObjectConst rate = in["rate"];
        mergeVal(rate, "target_load", next.rate.target_load);
        mergeVal(rate, "min_interval_ms", next.rate.min_interval_ms);
        mergeVal(rate, "max_interval_ms", next.rate.max_interval_ms);
        mergeVal(rate, "jitter_frac", next.rate.jitter_frac);
        mergeVal(rate, "duty_cycle_pct", next.rate.duty_cycle_pct);
    }
    if (in.containsKey("peers")) {
        JsonObjectConst peers = in["peers"];
        mergeVal(peers, "timeout_ms", next.peer_timeout_ms);
        mergeVal(peers, "announce_interval_ms", next.announce_interval_ms);
    }
    if (in.containsKey("msp")) {
        mergeVal(in["msp"].as<JsonObjectConst>(), "radar_interval_ms", next.msp_radar_interval_ms);
    }
    if (in.containsKey("gnss")) {
        mergeVal(in["gnss"].as<JsonObjectConst>(), "rate_hz", next.gnss_rate_hz);
    }
    if (in.containsKey("radios")) {
        JsonObjectConst radios = in["radios"];
        mergeVal(radios, "espnow_enabled", next.radios.espnow_enabled);
        mergeVal(radios, "lora_enabled", next.radios.lora_enabled);
        mergeVal(radios, "lora_power_dbm", next.radios.lora_power_dbm);
    }
    if (in.containsKey("wifi")) {
        JsonObjectConst wifi = in["wifi"];
        mergeVal(wifi, "ap", next.wifi.ap);
        copyStr(wifi["ssid"], next.wifi.ssid, kMaxSsidLen);
        copyStr(wifi["psk"], next.wifi.psk, kMaxPskLen);
        copyStr(wifi["ap_psk"], next.wifi.ap_psk, kMaxPskLen);
        mergeVal(wifi, "channel", next.wifi.channel);
    }
    if (in.containsKey("sim")) {
        mergeVal(in["sim"].as<JsonObjectConst>(), "enabled", next.sim.enabled);
    }
    if (in.containsKey("follow")) {
        mergeFollow(in["follow"], next.follow);
    }

    if (!configValidate(next, err)) {
        return false;  // cfg untouched
    }
    cfg = next;
    return true;
}

}  // namespace ff
