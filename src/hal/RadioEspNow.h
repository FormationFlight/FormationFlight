#pragma once
#include "radio_hub.h"
#include "ring_buffer.h"
#include "rx_frame.h"

namespace ff {

// RadioDriver for 2.4 GHz ESP-NOW broadcast (ESP32 and ESP8266).
//
// Unlike the LoRa drivers, ESP-NOW delivers received frames from a callback that
// runs in a different context (the WiFi task / SYS context), not the main loop.
// The callback pushes straight into the lock-free SpscRing; serviceRx() is a
// no-op and the loop drains via popRx(). This is exactly the producer/consumer
// split the ring was built for.
class RadioEspNow : public RadioDriver {
public:
    // WiFi must already be up, in a mode that has an AP interface, and on the
    // channel every node in the group agrees on. See wifiBringUp() in
    // hal/WebServer.h: ESP-NOW binds to that interface, so bringing WiFi up
    // afterwards (or switching to plain station mode) silently kills this link.
    // `channel` must match the one the AP was started on.
    bool begin(uint8_t channel);

    void transmit(const uint8_t* data, size_t len) override;
    double airtimeMs(size_t payload_len) const override;
    void serviceRx() override {}  // frames arrive asynchronously via the callback
    bool popRx(RxFrame& out) override { return rx_.pop(out); }
    const char* name() const override { return "ESPNOW"; }

    uint32_t rxDropped() const override { return rx_.dropped(); }
    uint32_t txDropped() const override { return tx_failed_; }

    // Called from the static ESP-NOW receive callback.
    void ingest(const uint8_t* data, int len);

private:
    uint32_t tx_failed_ = 0;
    SpscRing<RxFrame, 8> rx_;
};

}  // namespace ff
