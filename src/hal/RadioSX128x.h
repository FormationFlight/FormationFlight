#pragma once
#ifdef LORA_FAMILY_SX128X

#include <RadioLib.h>

#include "radio_hub.h"
#include "ring_buffer.h"
#include "rx_frame.h"
#include "tx_slot.h"

namespace ff {

// RadioDriver for the Semtech SX1280/SX1281 (2.4 GHz LoRa), as used on ExpressLRS
// receivers. Half-duplex: sits in continuous receive, briefly enters transmit for
// each beacon, then returns to receive. The DIO1 interrupt only sets a flag (kept
// in IRAM, ESP8266-safe); the actual SPI read and timestamping happen loop-side
// in serviceRx().
class RadioSX128x : public RadioDriver {
public:
    bool begin();

    // IRadio / RadioDriver
    void transmit(const uint8_t* data, size_t len) override;
    double airtimeMs(size_t payload_len) const override;
    void serviceRx() override;
    bool popRx(RxFrame& out) override { return rx_.pop(out); }
    const char* name() const override { return "SX128x"; }

    Info info() const override;
    uint32_t rxDropped() const override { return rx_.dropped(); }
    uint32_t txDropped() const override { return tx_dropped_; }
    uint32_t txDeferred() const override { return pending_.deferred(); }
    uint32_t txTimeouts() const override { return tx_timeouts_; }
    void onDioIsr() { dio_pending_ = true; }

private:
    // If a transmit-done interrupt is ever missed the radio would sit in
    // transmit forever and the link would simply stop, with nothing in the
    // counters to say why. Recover after this long instead.
    static constexpr uint32_t kTxTimeoutMs = 500;

    SX1281* radio_ = nullptr;
    volatile bool dio_pending_ = false;
    bool transmitting_ = false;
    uint32_t tx_start_ms_ = 0;
    uint32_t tx_dropped_ = 0;
    float last_snr_db_ = 0.0f;
    bool have_snr_ = false;
    uint32_t tx_timeouts_ = 0;
    // One frame deep, so a beacon and an announce arriving in the same loop
    // iteration cost the second one an airtime of latency instead of costing it
    // its existence. See tx_slot.h for why it is not deeper.
    TxSlot pending_;

    // Starts the frame waiting in pending_, if there is one. Returns false when
    // the slot was empty and the caller should put the radio back into receive:
    // startReceive() after a startTransmit() would abort the transmission.
    bool sendPending();
    void startTx(const uint8_t* data, size_t len);
    // The interrupt half of serviceRx(), split out so serviceRx() owns the one
    // decision about what the radio does next.
    void serviceIrq();
    SpscRing<RxFrame, 8> rx_;
};

}  // namespace ff

#endif  // LORA_FAMILY_SX128X
