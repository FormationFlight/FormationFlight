#pragma once
//
// The node's persisted configuration.
//
// One JSON document, held in RAM as this struct and written to a file by
// hal/ConfigStore. v1 kept a packed C struct in EEPROM, which meant every added
// field was a layout migration, the version byte had to be hand-bumped, and the
// whole thing was unreadable without the matching firmware. JSON costs a few
// hundred bytes of flash for the parser (already linked for the web API) and
// buys a config you can read, diff, back up and restore.
//
// Two rules the schema is built around, both learned the hard way:
//
//   - A partial update must never destroy fields it did not mention. Everything
//     here merges onto the live config (configMergeJson); a POST carrying one
//     key changes one key. The alternative silently wipes WiFi credentials the
//     first time a script posts a single setting.
//   - An unparseable file is not the same as a missing file. Missing means first
//     boot and defaults get written; unparseable means something is wrong and
//     the file is left alone to be recovered, with defaults used for this boot
//     only. See ConfigStore.
//
// Key names are the wire contract shared with the web UI, so they are stable:
// rename one and you have broken every saved config in the field.
//
#include <ArduinoJson.h>

#include <cstdint>

#include "follow.h"
#include "peer_table.h"
#include "rate_control.h"

namespace ff {

// Bumped only for a change no merge can absorb. Adding a key is not such a
// change: an old file simply leaves the new key at its default.
constexpr uint16_t kConfigVersion = 1;

constexpr size_t kMaxPassphraseLen = 32;
constexpr size_t kMaxSsidLen = 32;
constexpr size_t kMaxPskLen = 63;

// The passphrase that means "no encryption". Spelled out rather than an empty
// string, because an empty passphrase is a valid (if pointless) group key and
// the two must not be confused: "" still encrypts, with a key everyone knows.
constexpr const char* kCryptoDisabledPassphrase = "none";

struct NodeSettings {
    char name[kMaxNameLen + 1] = {0};  // empty = derive from the UID at boot
    // Track peers but never transmit: a ground station, or a receiver being
    // used purely as a display.
    bool listen_only = false;
};

struct SecuritySettings {
    // Group passphrase. Everyone who should see each other sets the same one.
    // Empty means the default key: interoperable out of the box, no secrecy.
    // The literal "none" disables the cipher entirely (bench use only).
    char passphrase[kMaxPassphraseLen + 1] = {0};
};

struct RadioSettings {
    bool espnow_enabled = true;
    bool lora_enabled = true;
    // Transmit power in dBm. Clamped by the driver to what the part supports;
    // 0 means "leave the compiled-in target default alone".
    int16_t lora_power_dbm = 0;
};

struct WifiSettings {
    bool ap = true;  // false = join an existing network
    char ssid[kMaxSsidLen + 1] = {0};
    char psk[kMaxPskLen + 1] = {0};
    // Password for our own AP. Empty leaves the AP open, which is the v1
    // behaviour and fine on a field bench.
    char ap_psk[kMaxPskLen + 1] = {0};
    // WiFi channel for our AP, and therefore for ESP-NOW: the two share one
    // radio and cannot be on different channels. Every node that should hear
    // every other over ESP-NOW must agree on this. 1, 6 and 11 are the
    // non-overlapping choices in 2.4 GHz.
    uint8_t channel = 1;
};

struct SimSettings {
    // Inject simulated peers on a virtual radio, for bench and HITL testing.
    // Never persisted as true by accident: it is written like any other field,
    // but the UI and the status view both shout while it is on.
    bool enabled = false;
};

struct Settings {
    uint16_t version = kConfigVersion;
    NodeSettings node;
    SecuritySettings security;
    RateConfig rate;
    uint32_t peer_timeout_ms = 6000;
    uint32_t announce_interval_ms = 2000;
    uint32_t msp_radar_interval_ms = 100;
    uint16_t gnss_rate_hz = 10;
    RadioSettings radios;
    WifiSettings wifi;
    FollowConfig follow;
    SimSettings sim;
};

// Serializes the whole config. `redact_secrets` replaces the group passphrase
// and WiFi passwords with a placeholder, for the GET the web UI uses: the
// browser never needs the secrets back, and a config fetched over an open AP
// should not hand them to whoever is listening.
void configToJson(const Settings& cfg, JsonObject out, bool redact_secrets = false);

// The placeholder written in place of a secret, and recognised on the way back
// in as "leave this field alone".
constexpr const char* kRedactedSecret = "••••••••";

// Applies only the keys present in `in` on top of `cfg`, then validates the
// result. On failure `cfg` is left exactly as it was and *err points at a static
// message. Unknown keys are ignored, so a newer UI talking to older firmware
// degrades instead of failing.
bool configMergeJson(JsonObjectConst in, Settings& cfg, const char** err = nullptr);

// Validates a whole config. Shares every rule with configMergeJson.
bool configValidate(const Settings& cfg, const char** err = nullptr);

}  // namespace ff
