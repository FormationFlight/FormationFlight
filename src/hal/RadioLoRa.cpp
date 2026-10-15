#if defined(LORA_FAMILY_SX127X) || defined(LORA_FAMILY_SX128X) || defined(LORA_FAMILY_LR1121)

#include "RadioLoRa.h"

#include <Arduino.h>
#include <SPI.h>

#include <cstring>

#include "airtime.h"
#include "log.h"

namespace ff {

namespace {

// One interrupt per radio on the board, whichever RadioLoRa owns it: a
// dual-band board has two instances, one radio each.
struct IsrSlot {
    RadioLoRa* owner = nullptr;
    uint8_t index = 0;
};
IsrSlot g_isr[2];

void IRAM_ATTR isr0() {
    if (g_isr[0].owner != nullptr) {
        g_isr[0].owner->onIsr(g_isr[0].index);
    }
}
void IRAM_ATTR isr1() {
    if (g_isr[1].owner != nullptr) {
        g_isr[1].owner->onIsr(g_isr[1].index);
    }
}

uint32_t rlPin(int pin) { return pin < 0 ? RADIOLIB_NC : static_cast<uint32_t>(pin); }

void outputLow(int pin) {
    if (pin != kPinNone) {
        pinMode(pin, OUTPUT);
        digitalWrite(pin, LOW);
    }
}

void write(int pin, bool high) {
    if (pin != kPinNone) {
        digitalWrite(pin, high ? HIGH : LOW);
    }
}

// A path nobody has heard anything on yet. Higher than any real RSSI, so an
// untried antenna gets tried; see chooseAntenna().
constexpr float kUntried = 1000.0f;
// How fast a path's average follows what it hears.
constexpr float kRssiAlpha = 0.3f;
// How far a path's average drifts each time a frame is heard somewhere else:
// down for a second radio that has gone deaf, up for an idle antenna, so that
// it gets tried again rather than being written off on old evidence.
constexpr float kRssiDriftDb = 0.5f;
// Two copies of the same frame are the same frame if they land this close.
constexpr uint32_t kDuplicateWindowMs = 50;

#if defined(LORA_FAMILY_LR1121)
// ELRS' TCXO voltage codes (SetTcxoMode), in volts.
constexpr float kTcxoVolts[8] = {1.6f, 1.7f, 1.8f, 2.2f, 2.4f, 2.7f, 3.0f, 3.3f};
// ELRS' SetDioAsRfSwitch default, for a layout that gives none.
constexpr uint8_t kDefaultRfsw[8] = {0b00001111, 0b00000000, 0b00000100, 0b00001000,
                                     0b00001000, 0b00000010, 0,          0b00000001};
#endif

const char* chipName() {
#if defined(LORA_FAMILY_LR1121)
    return "LR1121";
#elif defined(LORA_FAMILY_SX128X)
    return "SX1280";
#else
    return "SX127x";
#endif
}

bool sameFrame(const RxFrame& a, const RxFrame& b) {
    return a.len == b.len && std::memcmp(a.data, b.data, a.len) == 0;
}

}  // namespace

bool RadioLoRa::begin(const HardwareLayout& hw, const LoraBand& band, int requested_dbm,
                      uint8_t first, uint8_t max_radios) {
    hw_ = &hw;
    band_ = band;
    requested_dbm_ = requested_dbm;

    const char* err = nullptr;
    if (!loraBandValid(band_, &err)) {
        FF_LOGE("LoRa %u: %s", static_cast<unsigned>(band_.band), err);
        return false;
    }
    if (!hw.hasRadio(kBuildLoraChip, first)) {
        return false;
    }

    // Every radio on the board shares one SPI bus. Start it once, whichever
    // instance gets here first.
    static bool spi_started = false;
    if (!spi_started) {
#if defined(PLATFORM_ESP32)
        SPI.begin(hw.radio_sck, hw.radio_miso, hw.radio_mosi);
#else
        SPI.begin();  // ESP8266: fixed HSPI pins, which every ELRS board uses
#endif
        spi_started = true;
    }

    // Before the radios: ELRS holds it whenever a radio is in use, and a
    // radio behind an unpowered PA does initialise, it just never gets heard.
    if (hw.power_enable != kPinNone) {
        pinMode(hw.power_enable, OUTPUT);
        digitalWrite(hw.power_enable, HIGH);
    }

    for (uint8_t board = first; board < first + max_radios && board < 2; ++board) {
        if (!hw.hasRadio(kBuildLoraChip, board)) {
            continue;
        }
        const RadioPins& pins = hw.radio[board];
        // A second radio normally shares the first one's reset line. Reset it
        // again here and the first radio, already configured, is wiped.
        int rst = pins.rst;
        if (board > 0 && (rst == kPinNone || rst == hw.radio[0].rst)) {
            rst = kPinNone;
        }
        Path& p = chips_[count_];
        p.txen = pins.txen;
        p.rxen = pins.rxen;
        outputLow(p.txen);
        outputLow(p.rxen);

        g_isr[board].owner = this;
        g_isr[board].index = count_;
        if (!beginChip(p, pins, rst)) {
            FF_LOGE("%s #%u did not initialise", chipName(), static_cast<unsigned>(board + 1));
            g_isr[board].owner = nullptr;
            continue;
        }
        p.radio->setPacketReceivedAction(board == 0 ? isr0 : isr1);
        count_++;
    }
    if (count_ == 0) {
        return false;
    }

    // An antenna switch only means anything with one radio behind it.
    if (count_ == 1 && hw.ant_ctrl != kPinNone) {
        ant_pin_ = hw.ant_ctrl;
        outputLow(ant_pin_);
        ant_rssi_[0] = ant_rssi_[1] = kUntried;
    }

    snprintf(name_, sizeof(name_), "%s%s", chipName(), count_ > 1 ? "x2" : "");
    power_ = choosePower(hw, kBuildLoraChip, band_.highBand(), requested_dbm_, band_.max_dbm);
    applyPower();
    receiveAll();
    return true;
}

bool RadioLoRa::beginChip(Path& p, const RadioPins& pins, int rst) {
    const int irq = kBuildLoraChip == LoraChip::SX127x ? pins.dio0 : pins.dio1;
    Module* mod = new Module(rlPin(pins.nss), rlPin(irq), rlPin(rst), rlPin(pins.busy), SPI);
    p.radio = new Chip(mod);
    Chip* r = p.radio;

    const float freq_mhz = static_cast<float>(band_.frequency_hz) / 1e6f;
    // Power is set properly by applyPower() straight after; this is just a
    // value every chip accepts.
    constexpr int8_t kSafeDbm = 10;
    int16_t state;

#if defined(LORA_FAMILY_SX127X)
    state = r->begin(freq_mhz, band_.bandwidth_khz, band_.spreading_factor, band_.coding_rate,
                     band_.sync_word, kSafeDbm, band_.preamble_symbols, 0 /* AGC */);
    if (state != RADIOLIB_ERR_NONE) {
        return false;
    }
    r->setCurrentLimit(0);

#elif defined(LORA_FAMILY_SX128X)
    state = r->begin(freq_mhz, band_.bandwidth_khz, band_.spreading_factor, band_.coding_rate,
                     band_.sync_word, kSafeDbm, band_.preamble_symbols);
    if (state != RADIOLIB_ERR_NONE) {
        return false;
    }
    // RadioLib always switches the SX1280 to its DC-DC regulator, which only
    // works with the inductor fitted. ELRS boards say whether it is.
    if (!hw_->radio_dcdc) {
        const uint8_t ldo = RADIOLIB_SX128X_REGULATOR_LDO;
        r->getMod()->SPIwriteStream(RADIOLIB_SX128X_CMD_SET_REGULATOR_MODE, &ldo, 1);
    }
    r->setHighSensitivityMode(true);
    if (band_.long_interleaving) {
        state = r->setCodingRate(band_.coding_rate, true);
        if (state != RADIOLIB_ERR_NONE) {
            FF_LOGE("SX1280 rejected CR 4/%u with long interleaving",
                    static_cast<unsigned>(band_.coding_rate));
            return false;
        }
    }

#elif defined(LORA_FAMILY_LR1121)
    const float tcxo = (hw_->radio_tcxo >= 0 && hw_->radio_tcxo < 8)
                           ? kTcxoVolts[hw_->radio_tcxo]
                           : 0.0f;  // a crystal: RadioLib's default assumes a TCXO
    state = r->begin(freq_mhz, band_.bandwidth_khz, band_.spreading_factor, band_.coding_rate,
                     band_.sync_word, kSafeDbm, band_.preamble_symbols, tcxo);
    if (state != RADIOLIB_ERR_NONE) {
        return false;
    }
    if (hw_->radio_dcdc) {
        r->setRegulatorDCDC();
    } else {
        r->setRegulatorLDO();
    }
    // The LR1121 runs its own RF switches and PAs from DIO5-8 with this table:
    // nothing on the ESP side toggles anything per transmission.
    const uint8_t* sw =
        hw_->radio_rfsw_ctrl_count == 8 ? hw_->radio_rfsw_ctrl : kDefaultRfsw;
    state = r->setDioAsRfSwitch(sw[0], sw[1], sw[2], sw[3], sw[4], sw[5], sw[6], sw[7]);
    if (state != RADIOLIB_ERR_NONE) {
        FF_LOGE("LR1121 rejected its RF switch table (%d)", state);
        return false;
    }
    r->setRxBoostedGainMode(true);
    if (band_.long_interleaving) {
        state = r->setCodingRate(band_.coding_rate, true);
        if (state != RADIOLIB_ERR_NONE) {
            FF_LOGE("LR1121 rejected CR 4/%u with long interleaving",
                    static_cast<unsigned>(band_.coding_rate));
            return false;
        }
    }
#endif
    return true;
}

void RadioLoRa::applyPower() {
    for (uint8_t i = 0; i < count_; ++i) {
        Chip* r = chips_[i].radio;
        int16_t state;
#if defined(LORA_FAMILY_SX127X)
        state = r->setOutputPower(power_.chip_dbm, hw_->radio_rfo_hf);
#elif defined(LORA_FAMILY_SX128X)
        state = r->setOutputPower(power_.chip_dbm);
#elif defined(LORA_FAMILY_LR1121)
        // Below 2.4 GHz the layout picks the PA: radio_rfo_hf means the
        // low-power one. At 2.4 GHz there is only the HF PA.
        state = band_.highBand() ? r->setOutputPower(power_.chip_dbm)
                                 : r->setOutputPower(power_.chip_dbm, !hw_->radio_rfo_hf);
#endif
        if (state != RADIOLIB_ERR_NONE) {
            FF_LOGE("%s rejected %d dBm (%d)", chipName(), static_cast<int>(power_.chip_dbm),
                    state);
        }
    }
}

void RadioLoRa::setPower(int requested_dbm) {
    if (count_ == 0) {
        return;
    }
    requested_dbm_ = requested_dbm;
    const PowerChoice next =
        choosePower(*hw_, kBuildLoraChip, band_.highBand(), requested_dbm_, band_.max_dbm);
    if (next.chip_dbm == power_.chip_dbm && next.output_dbm == power_.output_dbm) {
        return;
    }
    power_ = next;
    if (transmitting_) {
        // Changing the PA setting mid-frame is not something to find out about
        // in the air. serviceRx() applies it once the frame is out.
        power_pending_ = true;
        return;
    }
    for (uint8_t i = 0; i < count_; ++i) {
        chips_[i].radio->standby();
    }
    applyPower();
    receiveAll();
}

void RadioLoRa::frontEndRx() {
    for (uint8_t i = 0; i < count_; ++i) {
        write(chips_[i].txen, false);
        write(chips_[i].rxen, true);
    }
}

void RadioLoRa::frontEndTx(uint8_t index) {
    // Every LNA off before any PA on: the other radio's LNA is centimetres from
    // an antenna about to put out a watt.
    for (uint8_t i = 0; i < count_; ++i) {
        write(chips_[i].rxen, false);
        write(chips_[i].txen, false);
    }
    write(chips_[index].txen, true);
}

void RadioLoRa::receiveAll() {
    frontEndRx();
    for (uint8_t i = 0; i < count_; ++i) {
        chips_[i].radio->startReceive();
    }
}

void RadioLoRa::transmit(const uint8_t* data, size_t len) {
    if (count_ == 0) {
        return;
    }
    if (transmitting_) {
        // A previous frame is still on the air. Calling startTransmit() again
        // here would abandon it mid-packet and put a truncated frame on the
        // channel, which is worse than sending nothing. The Node beacons each
        // radio independently and announces fan out to all of them, so two
        // sends CAN land in the same loop iteration.
        //
        // So it waits its turn instead. Only a third frame arriving inside the
        // same airtime is a real drop.
        if (!pending_.push(data, len)) {
            tx_dropped_++;
        }
        return;
    }
    startTx(data, len);
}

void RadioLoRa::startTx(const uint8_t* data, size_t len) {
    transmitting_ = true;
    tx_start_ms_ = millis();
    // The other radio would only be listening to us, through an LNA that is
    // about to be switched off. Park it; it goes back to receive with the rest.
    for (uint8_t i = 0; i < count_; ++i) {
        if (i != tx_index_) {
            chips_[i].radio->standby();
        }
    }
    frontEndTx(tx_index_);
    // RadioLib writes the payload into the chip's FIFO here and returns, so the
    // caller's buffer does not have to outlive this call -- which is what lets
    // sendPending() clear the slot immediately afterwards.
    chips_[tx_index_].radio->startTransmit(data, len);
}

bool RadioLoRa::sendPending() {
    if (pending_.empty()) {
        return false;
    }
    startTx(pending_.data(), pending_.size());
    pending_.clear();
    return true;
}

void RadioLoRa::notePathRssi(float& avg, float rssi) {
    if (avg == kUntried || avg <= -199.0f) {
        avg = rssi;
    } else {
        avg += kRssiAlpha * (rssi - avg);
    }
}

void RadioLoRa::chooseAntenna() {
    // Only between frames, and only towards an antenna that has been doing
    // better -- or has not been tried lately, which the drift arranges.
    const uint8_t other = ant_ ^ 1;
    if (ant_rssi_[other] > ant_rssi_[ant_]) {
        ant_ = other;
        write(ant_pin_, ant_ != 0);
    }
}

void RadioLoRa::serviceRx() {
    if (count_ == 0) {
        return;
    }

    bool tx_finished = false;
    bool restart[2] = {false, false};

    // Watchdog first, and outside the interrupt shortcut: a missed
    // transmit-done interrupt leaves transmitting_ stuck true, every later
    // transmit is then queued behind it, and the radio goes quiet with nothing
    // but the counters climbing to say so.
    if (transmitting_ && (millis() - tx_start_ms_) > kTxTimeoutMs) {
        tx_timeouts_++;
        transmitting_ = false;
        chips_[tx_index_].radio->finishTransmit();
        tx_finished = true;
    }

    RxFrame got[2];
    uint8_t got_from[2] = {0, 0};
    uint8_t n = 0;

    for (uint8_t i = 0; i < count_; ++i) {
        Path& p = chips_[i];
        if (!p.irq) {
            continue;
        }
        p.irq = false;
        Chip* r = p.radio;

        if (transmitting_ && i == tx_index_) {
            if (r->checkIrq(RADIOLIB_IRQ_TX_DONE)) {
                r->finishTransmit();
                transmitting_ = false;
                tx_finished = true;
            }
            continue;
        }

        restart[i] = true;
        if (!r->checkIrq(RADIOLIB_IRQ_RX_DONE) || r->checkIrq(RADIOLIB_IRQ_CRC_ERR)) {
            continue;
        }
        RxFrame& f = got[n];
        f = RxFrame{};
        size_t len = r->getPacketLength();
        if (len > sizeof(f.data)) {
            len = sizeof(f.data);
        }
        if (r->readData(f.data, len) != RADIOLIB_ERR_NONE) {
            continue;
        }
        f.timestamp_ms = millis();
        f.len = static_cast<uint8_t>(len);
        // What the antenna saw, not what an LNA in front of the radio made of
        // it: peers compare RSSI across boards that have one and boards that
        // do not.
        const float rssi = r->getRSSI() - hw_->power_lna_gain;
        f.rssi = static_cast<int16_t>(rssi);
        last_snr_db_ = r->getSNR();
        have_snr_ = true;
        got_from[n] = i;
        n++;
    }

    // Path bookkeeping: which radio or antenna has been hearing best.
    for (uint8_t k = 0; k < n; ++k) {
        if (count_ > 1) {
            notePathRssi(chips_[got_from[k]].rssi_avg, got[k].rssi);
        } else if (ant_pin_ != kPinNone) {
            notePathRssi(ant_rssi_[ant_], got[k].rssi);
            float& idle = ant_rssi_[ant_ ^ 1];
            if (idle != kUntried) {
                idle += kRssiDriftDb;
            }
        }
    }
    if (count_ > 1 && n > 0) {
        for (uint8_t i = 0; i < count_; ++i) {
            bool heard = false;
            for (uint8_t k = 0; k < n; ++k) {
                heard = heard || got_from[k] == i;
            }
            if (!heard && chips_[i].rssi_avg > -199.0f) {
                chips_[i].rssi_avg -= kRssiDriftDb;
            }
        }
        tx_index_ = chips_[1].rssi_avg > chips_[0].rssi_avg ? 1 : 0;
    }
    if (n > 0 && ant_pin_ != kPinNone) {
        chooseAntenna();
    }

    // Both radios heard the same frame: keep the stronger copy.
    if (n == 2 && sameFrame(got[0], got[1])) {
        if (got[1].rssi > got[0].rssi) {
            got[0] = got[1];
        }
        n = 1;
        duplicates_++;
    }
    for (uint8_t k = 0; k < n; ++k) {
        // The other radio's copy, landing a service pass after the first.
        if (have_last_rx_ && sameFrame(got[k], last_rx_) &&
            got[k].timestamp_ms - last_rx_.timestamp_ms < kDuplicateWindowMs) {
            duplicates_++;
            continue;
        }
        rx_.push(got[k]);
        last_rx_ = got[k];
        have_last_rx_ = true;
    }

    if (transmitting_) {
        return;
    }
    if (tx_finished) {
        if (power_pending_) {
            applyPower();
            power_pending_ = false;
        }
        // The radio is idle and something just finished on it. Send the frame
        // that was held back, if there is one, and only otherwise go back to
        // listening: startReceive() after a startTransmit() aborts it.
        if (!sendPending()) {
            receiveAll();
        }
        return;
    }
    // Only the radios that just finished something. The other one may be
    // halfway through receiving its own copy of the same frame.
    for (uint8_t i = 0; i < count_; ++i) {
        if (restart[i]) {
            chips_[i].radio->startReceive();
        }
    }
}

RadioLoRa::Info RadioLoRa::info() const {
    Info i;
    i.frequency_hz = band_.frequency_hz;
    i.bandwidth_khz = band_.bandwidth_khz;
    i.spreading_factor = band_.spreading_factor;
    i.coding_rate = band_.coding_rate;
    i.power_dbm = power_.output_dbm;
    i.has_snr = have_snr_;
    i.last_snr_db = last_snr_db_;
    return i;
}

double RadioLoRa::airtimeMs(size_t payload_len) const {
    return loraAirtimeMs(loraAirtimeParams(band_), payload_len);
}

}  // namespace ff

#endif
