//
// FormationFlight v2 firmware entry point.
//
// The v1 cooperative phase machine and its sys/curr/cfg globals are gone. The
// application lives in ff::Node (host-tested); this file is only the thin hardware
// wiring: build the radio drivers, group them in a RadioHub, hand it to the Node,
// and in loop() service the radios and pump the scheduler.
//
// Because ALOHA removed slot timing, radios run simultaneously: every target gets
// ESP-NOW, and LoRa targets additionally get their SX127x/SX128x radio. A node
// thus bridges short-range 2.4 GHz ESP-NOW and long-range LoRa at once.
//
#include <Arduino.h>

#include "hal/MspLocationSource.h"
#include "hal/MspRadarOutput.h"
#include "hal/PassthroughCrypto.h"
#include "hal/RadioEspNow.h"
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
#ifndef GNSS_RATE_HZ
#define GNSS_RATE_HZ 10  // highest reasonable u-blox rate; override per target
#endif
#endif

namespace {

ff::RadioHub g_hub;
ff::RadioEspNow g_espnow;
#ifdef LORA_FAMILY_SX128X
ff::RadioSX128x g_lora;
#endif
#ifdef LORA_FAMILY_SX127X
ff::RadioSX127x g_lora;
#endif

ff::PassthroughCrypto g_crypto;

// The MSP UART is always brought up: it is how this node pushes known peers out
// as MSP radar positions (to an attached FC's OSD, or to ground-station software
// in listen-only/GCS use) regardless of where OUR OWN position comes from.
ff::MspRadarOutput g_msp_radar_output;

// Our own position: a directly-attached GPS (auto-configured to a high rate) if
// the target has one -- on its own separate UART -- otherwise the flight
// controller's position read over the same MSP UART.
#ifdef GNSS_ENABLED
ff::DirectGpsLocationSource g_gps;
#else
ff::MspLocationSource g_location;
#endif

ff::Node* g_node = nullptr;

// Uniform [0,1) for ALOHA jitter. Arduino's PRNG is seeded per-device below.
float rng01(void*) { return static_cast<float>(random(0, 10000)) / 10000.0f; }

uint32_t deviceUid() {
#if defined(PLATFORM_ESP8266)
    return ESP.getChipId();
#else
    return static_cast<uint32_t>(ESP.getEfuseMac() & 0xFFFFFFFFu);
#endif
}

}  // namespace

void setup() {
    const uint32_t uid = deviceUid();
    randomSeed(micros() ^ uid);

    // ESP-NOW brings up WiFi; do it before the MSP UART so nothing races on boot.
    g_espnow.begin();
    g_hub.add(&g_espnow);

#if defined(LORA_FAMILY_SX128X) || defined(LORA_FAMILY_SX127X)
    g_lora.begin();
    g_hub.add(&g_lora);
#endif

    ff::NodeConfig cfg;
    cfg.uid = uid;
    snprintf(cfg.name, sizeof(cfg.name), "%03X", static_cast<unsigned>(uid & 0xFFFu));
    cfg.capabilities = ff::CAP_HAS_GPS | ff::CAP_HAS_MSP_FC;
#ifdef GCS_MODE
    // Listen-only / ground-station build: track peers, never transmit. Runtime
    // config (a web UI toggle, as v1 had) is Phase 3 work; this build flag is the
    // stop-gap until then. This is also the exact scenario the MSP radar output
    // fix above targets -- a GCS build has no other way to get peer data out.
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

    // Our own position: direct GPS on its own UART, or the FC over the MSP UART.
#ifdef GNSS_ENABLED
    g_gps.begin(GNSS_UART_INDEX, GNSS_PIN_RX, GNSS_PIN_TX, GNSS_RATE_HZ);
#else
    g_location.begin(mspStream);
#endif

#ifdef IO_LED_PIN
    pinMode(IO_LED_PIN, OUTPUT);
#endif

    ff::NodeDeps deps;
    deps.radios = &g_hub;
#ifdef GNSS_ENABLED
    deps.location = &g_gps;
#else
    deps.location = &g_location;
#endif
    deps.crypto = &g_crypto;
    deps.msp_radar_sink = &g_msp_radar_output;
    deps.rng = rng01;
    deps.rng_ctx = nullptr;

    g_node = new ff::Node(cfg, deps);
    g_node->begin(millis());
}

void loop() {
    // Service every radio and feed received frames (from any of them) to the Node.
    g_hub.service(*g_node);

    // Refresh our own position (direct GPS auto-config/parse, or MSP from the FC).
#ifdef GNSS_ENABLED
    g_gps.service();
#else
    g_location.service();
#endif

    // Drive the scheduler: beacons, announces, peer expiry, and MSP radar output
    // (the last of which runs on its own schedule regardless of listen_only/TX).
    g_node->poll(millis());
}
