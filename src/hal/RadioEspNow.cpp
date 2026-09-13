#include "RadioEspNow.h"

#include <Arduino.h>

#if defined(PLATFORM_ESP32)
#include <WiFi.h>
#include <esp_now.h>
#elif defined(PLATFORM_ESP8266)
#include <ESP8266WiFi.h>
#include <espnow.h>
#endif

namespace ff {

namespace {

uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
RadioEspNow* g_instance = nullptr;

#if defined(PLATFORM_ESP32)
void onRecv(const uint8_t* /*mac*/, const uint8_t* data, int len) {
    if (g_instance != nullptr) {
        g_instance->ingest(data, len);
    }
}
#elif defined(PLATFORM_ESP8266)
void onRecv(uint8_t* /*mac*/, uint8_t* data, uint8_t len) {
    if (g_instance != nullptr) {
        g_instance->ingest(data, len);
    }
}
#endif

}  // namespace

void RadioEspNow::ingest(const uint8_t* data, int len) {
    if (len <= 0) {
        return;
    }
    RxFrame frame{};
    size_t n = static_cast<size_t>(len);
    if (n > sizeof(frame.data)) {
        n = sizeof(frame.data);
    }
    for (size_t i = 0; i < n; i++) {
        frame.data[i] = data[i];
    }
    frame.len = static_cast<uint8_t>(n);
    frame.timestamp_ms = millis();
    frame.rssi = 0;  // ESP-NOW gives no per-packet RSSI here
    rx_.push(frame);
}

bool RadioEspNow::begin(uint8_t channel) {
    g_instance = this;

    // WiFi is brought up once, before any radio, by wifiBringUp(). This used to
    // start its own hidden AP here, which the web server then reconfigured out
    // from under it -- and in station mode removed the AP interface entirely,
    // taking ESP-NOW with it while every counter still read healthy.
    if (esp_now_init() != 0) {
        return false;
    }

#if defined(PLATFORM_ESP8266)
    esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
    esp_now_add_peer(kBroadcast, ESP_NOW_ROLE_COMBO, channel, nullptr, 0);
#elif defined(PLATFORM_ESP32)
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, kBroadcast, 6);
    peer.ifidx = WIFI_IF_AP;
    // 0 means "whatever channel the interface is on", which is what we want:
    // wifiBringUp() already put it on the configured one.
    peer.channel = 0;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
    (void)channel;
#endif

    esp_now_register_recv_cb(onRecv);
    return true;
}

void RadioEspNow::transmit(const uint8_t* data, size_t len) {
    if (esp_now_send(kBroadcast, const_cast<uint8_t*>(data), len) != 0) {
        // Queue full or the interface is down. Worth counting: it is the first
        // symptom of ESP-NOW having lost its interface, and otherwise invisible.
        tx_failed_++;
    }
}

double RadioEspNow::airtimeMs(size_t /*payload_len*/) const {
    // A short broadcast at the basic WiFi rate is well under a millisecond of PHY
    // time; use a small nominal value so ESP-NOW never dominates the hub's
    // max-airtime pacing (LoRa does).
    return 2.0;
}

}  // namespace ff
