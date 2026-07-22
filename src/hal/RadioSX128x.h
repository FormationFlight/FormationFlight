#pragma once
#ifdef LORA_FAMILY_SX128X

#include <RadioLib.h>

#include "RxFrame.h"
#include "node.h"
#include "ring_buffer.h"

namespace ff {

// IRadio adapter for the Semtech SX1280/SX1281 (2.4 GHz LoRa), as used on
// ExpressLRS receivers. Half-duplex: sits in continuous receive, briefly enters
// transmit for each beacon, then returns to receive. The DIO1 interrupt only
// sets a flag (kept in IRAM, ESP8266-safe); the actual SPI read and timestamping
// happen in serviceRx() on the main loop.
class RadioSX128x : public IRadio {
public:
    bool begin();

    // IRadio
    void transmit(const uint8_t* data, size_t len) override;
    double airtimeMs(size_t payload_len) const override;

    // Called every main-loop iteration: if a DIO event is pending, handle it
    // (read a received packet into the RX ring, or finish a transmit).
    void serviceRx();

    // Drain one received frame. Returns false when empty.
    bool popRx(RxFrame& out) { return rx_.pop(out); }

    uint32_t rxDropped() const { return rx_.dropped(); }

    // ISR hook (invoked from the RadioLib DIO callback).
    void onDioIsr() { dio_pending_ = true; }

private:
    SX1281* radio_ = nullptr;
    volatile bool dio_pending_ = false;
    bool transmitting_ = false;
    SpscRing<RxFrame, 8> rx_;
};

}  // namespace ff

#endif  // LORA_FAMILY_SX128X
