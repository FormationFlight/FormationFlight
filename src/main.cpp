//
// FormationFlight v2 firmware entry point.
//
// The v1 cooperative phase machine (MODE_START -> HOST_SCAN -> OTA_SCAN ->
// OTA_SYNC -> OTA_RX -> OTA_TX) and its sys/curr/cfg globals are gone. The
// application lives in ff::Node (host-tested); this file is only the thin
// hardware wiring:  build the adapters, hand them to the Node, and in loop()
// service the radio and pump the scheduler.
//
#include <Arduino.h>

#include "node.h"

#if !defined(LORA_FAMILY_SX128X)
#error "v2 firmware currently supports only SX128x (ExpressLRS 2.4GHz) targets; \
other radio adapters (SX127x, ESP-NOW) are not yet ported."
#endif

#include "hal/MspLocationSource.h"
#include "hal/PassthroughCrypto.h"
#include "hal/RadioSX128x.h"

namespace {

ff::RadioSX128x g_radio;
ff::PassthroughCrypto g_crypto;
ff::MspLocationSource g_location;
ff::Node* g_node = nullptr;

// Uniform [0,1) for ALOHA jitter. Arduino's PRNG is seeded per-device below, so
// different nodes draw different sequences (enough to decorrelate transmissions).
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

    g_radio.begin();

    ff::NodeConfig cfg;
    cfg.uid = uid;
    snprintf(cfg.name, sizeof(cfg.name), "%03X", static_cast<unsigned>(uid & 0xFFFu));
    cfg.capabilities = ff::CAP_HAS_GPS | ff::CAP_HAS_MSP_FC;

    ff::NodeDeps deps;
    deps.radio = &g_radio;
    deps.location = &g_location;
    deps.crypto = &g_crypto;
    deps.rng = rng01;
    deps.rng_ctx = nullptr;

    g_node = new ff::Node(cfg, deps);
    g_node->begin(millis());
}

void loop() {
    // Move any pending radio event into the RX ring, then feed frames to the Node.
    g_radio.serviceRx();
    ff::RxFrame frame;
    while (g_radio.popRx(frame)) {
        g_node->onReceive(frame.data, frame.len, frame.timestamp_ms, frame.rssi);
    }

    // Refresh our own position from the FC (rate-limited internally).
    g_location.service();

    // Drive the scheduler: beacons, announces, peer expiry.
    g_node->poll(millis());
}
