//
// FormationFlight v2 firmware entry point.
//
// The v1 cooperative phase machine and its sys/curr/cfg globals are gone. The
// application lives in ff::Node (host-tested); this file is only the thin
// hardware wiring: load the configuration, build the radio drivers, group them
// in a RadioHub, hand it to the Node, and in loop() service everything.
//
// Because ALOHA removed slot timing, radios run simultaneously: every target
// gets ESP-NOW, and LoRa targets additionally get their SX127x/SX128x radio. A
// node thus bridges short-range 2.4 GHz ESP-NOW and long-range LoRa at once.
//
#include <Arduino.h>

#include <cstring>

#include "config.h"
#include "crypto.h"
#include "follow.h"
#include "hal/BoardPower.h"
#include "hal/ConfigStore.h"
#include "hal/MspFcLink.h"
#include "hal/MspRadarOutput.h"
#include "hal/PassthroughCrypto.h"
#include "hal/RadioEspNow.h"
#include "hal/SimRadio.h"
#include "hal/WebServer.h"
#include "node.h"
#include "radio_hub.h"

#ifdef LORA_FAMILY_SX128X
#include "hal/RadioSX128x.h"
#endif
#ifdef LORA_FAMILY_SX127X
#include "hal/RadioSX127x.h"
#endif
#ifdef GNSS_ENABLED
#include "hal/DirectGpsLocationSource.h"
#endif

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

namespace {

ff::Settings g_settings;
ff::ConfigStore g_store;
ff::BoardPower g_power;

ff::RadioHub g_hub;
ff::RadioEspNow g_espnow;
#ifdef LORA_FAMILY_SX128X
ff::RadioSX128x g_lora;
#endif
#ifdef LORA_FAMILY_SX127X
ff::RadioSX127x g_lora;
#endif
ff::SimRadio g_sim;

// The frame cipher, and the plaintext escape hatch for bench work. Which one is
// in use is decided once at boot from the configured passphrase.
ff::CcmCrypto g_ccm;
ff::PassthroughCrypto g_plaintext;
ff::ICrypto* g_crypto = nullptr;
bool g_crypto_enabled = true;

// The MSP UART is always brought up: it is how this node pushes known peers out
// as MSP radar positions (to an attached FC's OSD, or to ground-station software
// in listen-only/GCS use) regardless of where OUR OWN position comes from.
ff::MspRadarOutput g_msp_radar_output;

// The non-blocking link to the flight controller on that same UART: the FC's
// GPS fix and arm state for the beacon, and everything Follow reads and writes.
ff::MspFcLink g_fc;

#ifdef GNSS_ENABLED
ff::DirectGpsLocationSource g_gps;
#endif

ff::Node* g_node = nullptr;
ff::FollowController* g_follow = nullptr;
ff::WebServer g_web;

uint32_t g_uid = 0;
uint8_t g_wifi_channel = 1;

// What the configuration said at boot. Some settings cannot be applied to a
// running node -- the group key, and any radio that was never constructed --
// and the only way to know a POST changed one is to compare against this rather
// than against the live config, which the web handler has already mutated.
struct BootState {
    char passphrase[ff::kMaxPassphraseLen + 1] = {0};
    bool espnow_constructed = false;
    bool lora_constructed = false;
};
BootState g_boot;

// Uniform [0,1) for ALOHA jitter. Arduino's PRNG is seeded per-device below.
float rng01(void*) { return static_cast<float>(random(0, 10000)) / 10000.0f; }

uint32_t deviceUid() {
#if defined(PLATFORM_ESP8266)
    return ESP.getChipId();
#else
    return static_cast<uint32_t>(ESP.getEfuseMac() & 0xFFFFFFFFu);
#endif
}

// Everything that can be changed while flying, pushed into the live objects.
// Anything not handled here needs a reboot, and the web UI says so.
void applyLiveConfig(const ff::Settings& cfg) {
    if (g_follow != nullptr) {
        g_follow->applyConfig(cfg.follow, nullptr);
    }

    // A radio can only be switched on live if it was constructed at boot.
    // Turning one off always works.
    g_espnow.setEnabled(cfg.radios.espnow_enabled && g_boot.espnow_constructed);
    if (cfg.radios.espnow_enabled && !g_boot.espnow_constructed) {
        g_web.setRebootRequired();
    }
#if defined(LORA_FAMILY_SX128X) || defined(LORA_FAMILY_SX127X)
    g_lora.setEnabled(cfg.radios.lora_enabled && g_boot.lora_constructed);
    if (cfg.radios.lora_enabled && !g_boot.lora_constructed) {
        g_web.setRebootRequired();
    }
#endif

    // The simulator is always constructed, precisely so it can be switched on
    // and off from the bench without a reboot.
    g_sim.setEnabled(cfg.sim.enabled);
    if (!cfg.sim.enabled) {
        g_sim.clear();
    }

    // The group passphrase only takes effect on the cipher at boot: rekeying a
    // live node would strand it mid-flight from every peer that has not been
    // changed yet, which is a worse failure than waiting for a reboot.
    if (std::strcmp(cfg.security.passphrase, g_boot.passphrase) != 0) {
        g_web.setRebootRequired();
    }
}

}  // namespace

void setup() {
    g_uid = deviceUid();
    randomSeed(micros() ^ g_uid);

    // Power rails before anything that lives on them. On the T-Beam the GPS and
    // the LoRa radio are behind a PMIC and come up off, so bringing SPI up first
    // would be initialising an unpowered module.
    g_power.begin();

    // Configuration first: it decides which radios come up, and with what key.
    g_store.begin();
    g_store.load(g_settings);

    // Pick the cipher. An explicit "none" is the bench escape hatch; anything
    // else, including an empty passphrase, is encrypted.
    g_crypto_enabled =
        std::strcmp(g_settings.security.passphrase, ff::kCryptoDisabledPassphrase) != 0;
    if (g_crypto_enabled) {
        uint8_t key[ff::kAesKeySize];
        ff::deriveGroupKey(g_settings.security.passphrase, key);
        // The counter starts somewhere random rather than at zero: a node that
        // reboots repeatedly would otherwise replay the same counter sequence
        // under the same key, and CCM's security rests on never reusing a nonce.
        g_ccm.begin(g_uid, key, random(1, 0x40000000));
        g_crypto = &g_ccm;
    } else {
        g_crypto = &g_plaintext;
    }

    snprintf(g_boot.passphrase, sizeof(g_boot.passphrase), "%s", g_settings.security.passphrase);

    // WiFi first, once, and nothing may change its mode afterwards: ESP-NOW
    // binds to the AP interface this creates. Getting this order wrong kills the
    // 2.4 GHz link while every counter still reads healthy.
    g_wifi_channel = ff::wifiBringUp(g_settings, g_uid);

    if (g_settings.radios.espnow_enabled) {
        g_espnow.begin(g_wifi_channel);
        g_hub.add(&g_espnow);
        g_boot.espnow_constructed = true;
    }

#if defined(LORA_FAMILY_SX128X) || defined(LORA_FAMILY_SX127X)
    if (g_settings.radios.lora_enabled) {
        g_lora.begin();
        g_hub.add(&g_lora);
        g_boot.lora_constructed = true;
    }
#endif

    // The virtual radio is always present, disabled unless the config asks for
    // it. Constructing it unconditionally is what lets the simulator be turned
    // on from the web UI without a reboot, which is the whole point of having a
    // bench testing path.
    g_sim.begin(g_crypto_enabled ? &g_ccm : nullptr);
    g_sim.setEnabled(g_settings.sim.enabled);
    g_hub.add(&g_sim);

    ff::NodeConfig cfg;
    cfg.uid = g_uid;
    if (g_settings.node.name[0] != '\0') {
        snprintf(cfg.name, sizeof(cfg.name), "%s", g_settings.node.name);
    } else {
        snprintf(cfg.name, sizeof(cfg.name), "%03X", static_cast<unsigned>(g_uid & 0xFFFu));
    }
    cfg.capabilities = ff::CAP_HAS_GPS | ff::CAP_HAS_MSP_FC;
    cfg.rate = g_settings.rate;
    cfg.peer_timeout_ms = g_settings.peer_timeout_ms;
    cfg.announce_interval_ms = g_settings.announce_interval_ms;
    cfg.msp_radar_interval_ms = g_settings.msp_radar_interval_ms;
    cfg.listen_only = g_settings.node.listen_only;
#ifdef GCS_MODE
    // Ground-station build: listen only, whatever the stored config says.
    cfg.listen_only = true;
#endif

    // MSP UART: always brought up (see g_msp_radar_output above).
#if defined(PLATFORM_ESP32)
    Serial1.begin(115200, SERIAL_8N1, SERIAL_PIN_RX, SERIAL_PIN_TX);
    Stream& mspStream = Serial1;
#else
    Serial.begin(115200);
    Stream& mspStream = Serial;
#endif
    g_msp_radar_output.begin(mspStream, cfg.peer_timeout_ms);
    g_fc.begin(mspStream);

#ifdef GNSS_ENABLED
    g_gps.begin(GNSS_UART_INDEX, GNSS_PIN_RX, GNSS_PIN_TX, g_settings.gnss_rate_hz);
    ff::ILocationSource* location = &g_gps;
#else
    ff::ILocationSource* location = &g_fc;
#endif

#ifdef IO_LED_PIN
    pinMode(IO_LED_PIN, OUTPUT);
#endif

    ff::NodeDeps deps;
    deps.radios = &g_hub;
    deps.location = location;
    deps.crypto = g_crypto;
    deps.msp_radar_sink = &g_msp_radar_output;
    deps.rng = rng01;
    deps.rng_ctx = nullptr;

    g_node = new ff::Node(cfg, deps);
    g_node->begin(millis());

    if (!cfg.listen_only) {
        g_follow = new ff::FollowController(&g_node->peers(), location, &g_fc);
        g_follow->applyConfig(g_settings.follow, nullptr);
    }

    ff::WebDeps web;
    web.cfg = &g_settings;
    web.store = &g_store;
    web.node = g_node;
    web.hub = &g_hub;
    web.follow = g_follow;
    web.fc = &g_fc;
    web.sim = &g_sim;
    web.crypto = g_crypto_enabled ? &g_ccm : nullptr;
    web.location = location;
    web.fw_version = FIRMWARE_VERSION;
    web.uid = g_uid;
    web.wifi_channel = g_wifi_channel;
    web.power = &g_power;
    web.on_config_applied = applyLiveConfig;
    g_web.begin(web);
}

void loop() {
    const uint32_t now = millis();

    // A firmware upload is erasing and writing flash. On ESP8266 that stalls the
    // CPU for milliseconds at a time with interrupts disabled, so beaconing
    // through it achieves nothing except corrupt transmissions and a starved web
    // server. Hold the radio work still until the upload finishes; the node
    // reboots straight afterwards anyway.
    if (g_web.otaActive()) {
        g_web.loop(now);
        return;
    }

    // Manufacture any simulated traffic before the hub drains the radios, so
    // simulated frames are picked up in the same iteration they are produced.
    if (g_settings.sim.enabled) {
        g_sim.service(now);
    }

    // Service every radio and feed received frames (from any of them) to the Node.
    g_hub.service(*g_node);

    // Drain the FC link (fix, modes, Follow telemetry) and, if present, the
    // direct GPS. Both are pure byte pumps; neither ever waits.
    g_fc.service(now);
#ifdef GNSS_ENABLED
    g_gps.service();
#endif

    // Drive the scheduler: beacons, announces, peer expiry, and MSP radar output
    // (the last of which runs on its own schedule regardless of listen_only/TX).
    g_node->poll(now);

    // Follow runs its own control cycle at its configured rate.
    if (g_follow != nullptr) {
        g_follow->service(now);
    }

    g_web.loop(now);
}
