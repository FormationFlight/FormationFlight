#pragma once
//
// RadioHub: run several radios at once.
//
// Because ALOHA removed the slot timing that used to couple the medium to the
// schedule, a node can transmit and receive on multiple radios simultaneously --
// e.g. short-range 2.4 GHz ESP-NOW bridged to long-range sub-GHz LoRa. Frames
// received on any of them feed the same peer table, and the Node beacons on each
// independently (see IRadioSet in node.h): each radio's rate is sized from its own
// airtime, so LoRa slowing down under load does not throttle ESP-NOW.
//
// The hub implements IRadioSet, so the Node addresses radios by index. Pure and
// host-testable: concrete drivers are injected as RadioDriver pointers.
//
#include <cstddef>

#include "node.h"
#include "rx_frame.h"

namespace ff {

// A concrete radio: the transmit + airtime pair the hub exposes to the Node, plus
// the loop-facing servicing side the hub drives.
class RadioDriver {
public:
    virtual ~RadioDriver() = default;

    virtual void transmit(const uint8_t* data, size_t len) = 0;
    virtual double airtimeMs(size_t payload_len) const = 0;

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

class RadioHub : public IRadioSet {
public:
    bool add(RadioDriver* driver);
    RadioDriver* at(size_t i) const { return (i < count_) ? radios_[i] : nullptr; }

    // IRadioSet
    size_t radioCount() const override { return count_; }
    bool radioEnabled(size_t index) const override;
    double airtimeMs(size_t index, size_t payload_len) const override;
    void transmit(size_t index, const uint8_t* data, size_t len) override;

    // Service every radio and deliver received frames to the Node. Call each loop.
    void service(Node& node);

private:
    RadioDriver* radios_[kMaxRadios] = {nullptr};
    size_t count_ = 0;
};

}  // namespace ff
