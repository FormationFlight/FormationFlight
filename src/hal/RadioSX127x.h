#pragma once
#ifdef LORA_FAMILY_SX127X

#include <RadioLib.h>

#include "radio_hub.h"
#include "ring_buffer.h"
#include "rx_frame.h"

namespace ff {

// RadioDriver for the Semtech SX1276/SX1278 (sub-GHz LoRa) on LilyGo / Heltec /
// ELRS 900 hardware. Same half-duplex pattern as the SX128x driver: DIO0 ISR sets
// an IRAM flag, the SPI read and timestamp happen loop-side in serviceRx().
class RadioSX127x : public RadioDriver {
public:
    bool begin();

    void transmit(const uint8_t* data, size_t len) override;
    double airtimeMs(size_t payload_len) const override;
    void serviceRx() override;
    bool popRx(RxFrame& out) override { return rx_.pop(out); }
    const char* name() const override { return "SX127x"; }

    Info info() const override;
    uint32_t rxDropped() const override { return rx_.dropped(); }
    uint32_t txDropped() const override { return tx_dropped_; }
    void onDioIsr() { dio_pending_ = true; }

private:
#if LORA_BAND == 433
    SX1278* radio_ = nullptr;
#else
    SX1276* radio_ = nullptr;
#endif
    volatile bool dio_pending_ = false;
    bool transmitting_ = false;
    uint32_t tx_start_ms_ = 0;
    uint32_t tx_dropped_ = 0;
    float last_snr_db_ = 0.0f;
    bool have_snr_ = false;
    uint32_t tx_timeouts_ = 0;

    // If a transmit-done interrupt is ever missed the radio would sit in
    // transmit forever and the link would simply stop, with nothing in the
    // counters to say why. Recover after this long instead.
    static constexpr uint32_t kTxTimeoutMs = 500;
    SpscRing<RxFrame, 8> rx_;
};

}  // namespace ff

#endif  // LORA_FAMILY_SX127X
