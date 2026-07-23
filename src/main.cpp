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
ff::MspLocationSource g_location;
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

    // MSP link to the flight controller.
#if defined(PLATFORM_ESP32)
    Serial1.begin(115200, SERIAL_8N1, SERIAL_PIN_RX, SERIAL_PIN_TX);
    g_location.begin(Serial1);
#else
    Serial.begin(115200);
    g_location.begin(Serial);
#endif

#ifdef IO_LED_PIN
    pinMode(IO_LED_PIN, OUTPUT);
#endif

    ff::NodeConfig cfg;
    cfg.uid = uid;
    snprintf(cfg.name, sizeof(cfg.name), "%03X", static_cast<unsigned>(uid & 0xFFFu));
    cfg.capabilities = ff::CAP_HAS_GPS | ff::CAP_HAS_MSP_FC;

    ff::NodeDeps deps;
    deps.radios = &g_hub;
    deps.location = &g_location;
    deps.crypto = &g_crypto;
    deps.rng = rng01;
    deps.rng_ctx = nullptr;

    g_node = new ff::Node(cfg, deps);
    g_node->begin(millis());
}

void loop() {
    // Service every radio and feed received frames (from any of them) to the Node.
    g_hub.service(*g_node);

    // Refresh our own position from the FC (rate-limited internally).
    g_location.service();

    // Drive the scheduler: beacons, announces, peer expiry.
    g_node->poll(millis());
}
