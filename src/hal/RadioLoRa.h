#pragma once
// An LR1121 env adds LORA_FAMILY_LR1121 on top of the band section's default
// chip, and wins.
#if defined(LORA_FAMILY_LR1121)
#undef LORA_FAMILY_SX127X
#undef LORA_FAMILY_SX128X
#endif
#if defined(LORA_FAMILY_SX127X) || defined(LORA_FAMILY_SX128X) || defined(LORA_FAMILY_LR1121)

#include <RadioLib.h>

#include "hw_layout.h"
#include "lora_band.h"
#include "radio_hub.h"
#include "ring_buffer.h"
#include "rx_frame.h"
#include "tx_slot.h"

namespace ff {

// The chip this image drives. Still a compile-time choice: a unified build is
// per chip and band, exactly as ELRS' are.
#if defined(LORA_FAMILY_LR1121)
constexpr LoraChip kBuildLoraChip = LoraChip::LR1121;
#elif defined(LORA_FAMILY_SX128X)
constexpr LoraChip kBuildLoraChip = LoraChip::SX128x;
#else
constexpr LoraChip kBuildLoraChip = LoraChip::SX127x;
#endif

// The LoRa radio, for every chip and every board ExpressLRS describes: SX127x,
// SX1280 or LR1121; one radio or two; with or without a PA/LNA the ESP has to
// switch; with or without an antenna switch.
//
// Half-duplex. Every radio sits in receive; a frame goes out on one of them and
// the rest stand by until it is done. Each radio's interrupt only sets an IRAM
// flag; the SPI work and timestamping happen loop-side in serviceRx().
//
// Diversity, for two radios or one radio with an antenna switch, is the cheap
// kind that suits beacons from many peers: receive on everything, keep the
// strongest copy of a frame, and transmit on whichever path has been hearing
// best. There is no per-peer antenna choice, because there is no link to have
// one with.
class RadioLoRa : public RadioDriver {
public:
    // Drives the board's radios first .. first + max_radios - 1 (those the
    // layout has) on `band`. One instance takes both radios of a same-band
    // diversity receiver; a dual-band board gets one instance per radio.
    //
    // False when the layout has no radio for this chip, or it does not answer.
    // requested_dbm is radios.lora_power_dbm: 0 for the board default.
    bool begin(const HardwareLayout& hw, const LoraBand& band, int requested_dbm,
               uint8_t first = 0, uint8_t max_radios = 2);

    // Applies radios.lora_power_dbm to a running radio. Takes effect after the
    // frame on the air, if there is one.
    void setPower(int requested_dbm);

    void transmit(const uint8_t* data, size_t len) override;
    double airtimeMs(size_t payload_len) const override;
    void serviceRx() override;
    bool popRx(RxFrame& out) override { return rx_.pop(out); }
    const char* name() const override { return name_; }

    Info info() const override;
    uint32_t rxDropped() const override { return rx_.dropped(); }
    uint32_t txDropped() const override { return tx_dropped_; }
    uint32_t txDeferred() const override { return pending_.deferred(); }
    uint32_t txTimeouts() const override { return tx_timeouts_; }

    // How many radios answered, and how often diversity paid off: a frame
    // heard on more than one path, of which the stronger copy was kept.
    uint8_t radioCount() const { return count_; }
    uint32_t diversityDuplicates() const { return duplicates_; }

    void onIsr(uint8_t index) { chips_[index].irq = true; }

#if defined(LORA_FAMILY_LR1121)
    // RadioLib keeps SetDioAsRfSwitch to itself, behind a pin table aimed at
    // switches the MCU drives. ELRS layouts give the command's raw bytes, and
    // the LR1121 then drives its own front end, so hand them over as they are.
    class Chip : public LR1121 {
    public:
        using LR1121::LR1121;
        using LR11x0::setDioAsRfSwitch;
    };
#elif defined(LORA_FAMILY_SX128X)
    // RadioLib always selects the DC-DC regulator and offers no way back to
    // the LDO, so the regulator command goes straight through the Module.
    class Chip : public SX1281 {
    public:
        using SX1281::SX1281;
        using SX128x::getMod;
    };
#else
    // SX1276 for every SX127x board, whatever its band: RadioLib's SX1276
    // driver tunes 137-1020 MHz, and an SX1278 reports the same silicon
    // version, so one class covers 433, 868 and 915.
    using Chip = SX1276;
#endif

private:
    struct Path {
        Chip* radio = nullptr;
        volatile bool irq = false;
        int txen = kPinNone;
        int rxen = kPinNone;
        // Smoothed RSSI of what this radio (or antenna) has been hearing, for
        // choosing where to transmit. Starts low so the first path to hear
        // anything wins.
        float rssi_avg = -200.0f;
    };

    bool beginChip(Path& p, const RadioPins& pins, int rst);
    void applyPower();
    void frontEndRx();
    void frontEndTx(uint8_t index);
    void receiveAll();
    bool sendPending();
    void startTx(const uint8_t* data, size_t len);
    void notePathRssi(float& avg, float rssi);
    void chooseAntenna();

    Path chips_[2];
    uint8_t count_ = 0;
    uint8_t tx_index_ = 0;
    const HardwareLayout* hw_ = nullptr;
    LoraBand band_;
    char name_[16] = "LoRa";

    // One radio with an antenna switch: which antenna, and how each has been
    // doing.
    int ant_pin_ = kPinNone;
    uint8_t ant_ = 0;
    float ant_rssi_[2] = {-200.0f, -200.0f};

    PowerChoice power_;
    int requested_dbm_ = 0;
    bool power_pending_ = false;

    bool transmitting_ = false;
    uint32_t tx_start_ms_ = 0;
    uint32_t tx_dropped_ = 0;
    uint32_t tx_timeouts_ = 0;
    uint32_t duplicates_ = 0;
    float last_snr_db_ = 0.0f;
    bool have_snr_ = false;

    // The last frame handed up, so the same frame arriving on the other radio
    // a beat later is recognised as a copy rather than a second frame.
    RxFrame last_rx_{};
    bool have_last_rx_ = false;

    // One frame deep, so a beacon and an announce arriving in the same loop
    // iteration cost the second one an airtime of latency instead of costing it
    // its existence. See tx_slot.h for why it is not deeper.
    TxSlot pending_;

    // If a transmit-done interrupt is ever missed the radio would sit in
    // transmit forever and the link would simply stop, with nothing in the
    // counters to say why. Recover after this long instead.
    static constexpr uint32_t kTxTimeoutMs = 500;
    SpscRing<RxFrame, 8> rx_;
};

}  // namespace ff

#endif
