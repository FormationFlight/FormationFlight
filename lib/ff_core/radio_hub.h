#pragma once
//
// RadioHub: run several radios at once.
//
// Because ALOHA removed the slot timing that used to couple the medium to the
// schedule, a node can transmit and receive on multiple radios simultaneously --
// e.g. short-range 2.4 GHz ESP-NOW bridged to long-range sub-GHz LoRa. Every
// beacon goes out on all of them, and frames received on any of them feed the
// same peer table.
//
// The hub is itself an IRadio, so Node still sees a single radio and stays
// unchanged. It reports the *maximum* child airtime, so the ALOHA rate controller
// paces to the slowest medium (LoRa), which is the one that can actually
// congest.
//
// Pure and host-testable: concrete drivers are injected as RadioDriver pointers.
//
#include <cstddef>

#include "node.h"
#include "rx_frame.h"

namespace ff {

// A concrete radio: the Node-facing transmit side (IRadio) plus the loop-facing
// servicing side the hub drives.
class RadioDriver : public IRadio {
public:
    // Move any pending hardware event into the driver's RX ring.
    virtual void serviceRx() = 0;
    // Pop one received frame; false when none pending.
    virtual bool popRx(RxFrame& out) = 0;
    // Short identifier for status/telemetry (e.g. "ESPNOW", "SX127x").
    virtual const char* name() const = 0;

    bool enabled() const { return enabled_; }
    void setEnabled(bool e) { enabled_ = e; }

protected:
    bool enabled_ = true;
};

constexpr size_t kMaxRadios = 4;

class RadioHub : public IRadio {
public:
    bool add(RadioDriver* driver);
    size_t count() const { return count_; }
    RadioDriver* at(size_t i) const { return (i < count_) ? radios_[i] : nullptr; }

    // IRadio: fan the frame out to every enabled radio.
    void transmit(const uint8_t* data, size_t len) override;
    // IRadio: the largest airtime among enabled radios (paces rate control to the
    // slowest medium). Zero if no radio is enabled.
    double airtimeMs(size_t payload_len) const override;

    // Service every radio and deliver received frames to the Node. Call each loop.
    void service(Node& node);

private:
    RadioDriver* radios_[kMaxRadios] = {nullptr};
    size_t count_ = 0;
};

}  // namespace ff
