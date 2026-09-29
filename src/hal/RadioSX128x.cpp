#ifdef LORA_FAMILY_SX128X

#include "RadioSX128x.h"

#include <Arduino.h>
#include <SPI.h>

#include "airtime.h"
#include "protocol.h"

namespace ff {

// Compile-time band-edge check. The channel, bandwidth included, must sit wholly
// inside 2400-2483.5 MHz. This exists because the default here was once
// 2400.000 MHz, which put half the channel below the band edge.
namespace {
constexpr double kCentreHz = static_cast<double>(LORA_FREQUENCY);
constexpr double kHalfBwHz = LORA_BW_KHZ * 1000.0 / 2.0;
constexpr double kBandLowHz = 2400.0e6;
constexpr double kBandHighHz = 2483.5e6;
static_assert(kCentreHz - kHalfBwHz >= kBandLowHz,
              "LoRa channel extends below 2400 MHz; check LORA_FREQUENCY and LORA_BW_KHZ");
static_assert(kCentreHz + kHalfBwHz <= kBandHighHz,
              "LoRa channel extends above 2483.5 MHz; check LORA_FREQUENCY and LORA_BW_KHZ");
// A fixed-channel system in the US needs a 6 dB bandwidth of at least 500 kHz
// (FCC 15.247(a)(2)). ExpressLRS uses 406.25 kHz legally because it hops;
// nothing in v2 hops, so that exemption is not available here.
static_assert(LORA_BW_KHZ >= 500.0,
              "a fixed-channel 2.4 GHz system needs at least 500 kHz of bandwidth");
}  // namespace

namespace {

// Modulation comes from the band definition in platformio.ini, because the
// legal channel width is a property of the band, not of the chip. See the
// comments there, in particular why a fixed-channel system needs at least
// 500 kHz where a hopping one like ExpressLRS does not.
#ifndef LORA_BW_KHZ
#define LORA_BW_KHZ 812.5
#endif
#ifndef LORA_SF
#define LORA_SF 5
#endif
#ifndef LORA_CR
#define LORA_CR 6
#endif
constexpr float kBandwidthKHz = static_cast<float>(LORA_BW_KHZ);
constexpr uint8_t kSpreadingFactor = LORA_SF;
constexpr uint8_t kCodingRate = LORA_CR;
constexpr uint8_t kSyncWord = 0x17;
constexpr uint16_t kPreambleSymbols = 12;

RadioSX128x* g_instance = nullptr;

void IRAM_ATTR dioIsrTrampoline() {
    if (g_instance != nullptr) {
        g_instance->onDioIsr();
    }
}

}  // namespace

bool RadioSX128x::begin() {
    g_instance = this;

#if defined(PLATFORM_ESP32)
    SPI.begin(LORA_PIN_SCK, LORA_PIN_MISO, LORA_PIN_MOSI, LORA_PIN_CS);
#else
    SPI.begin();  // ESP8266 uses fixed HSPI pins
#endif

    radio_ = new SX1281(new Module(LORA_PIN_CS, LORA_PIN_DIO, LORA_PIN_RST, LORA_PIN_BUSY));
    radio_->reset(false);

    const float freq_mhz = static_cast<float>(LORA_FREQUENCY) / 1000000.0f;
    int state = radio_->begin(freq_mhz, kBandwidthKHz, kSpreadingFactor, kCodingRate,
                              kSyncWord, LORA_POWER, kPreambleSymbols);
    if (state != RADIOLIB_ERR_NONE) {
        return false;
    }
    // The SX128x driver wants some of these applied again after begin().
    radio_->setHighSensitivityMode(true);
    radio_->setFrequency(freq_mhz);
    radio_->setBandwidth(kBandwidthKHz);
    radio_->setSpreadingFactor(kSpreadingFactor);
    radio_->setCodingRate(kCodingRate, true /* long interleaving */);
    radio_->setSyncWord(kSyncWord);
    radio_->setOutputPower(LORA_POWER);
    radio_->setPreambleLength(kPreambleSymbols);
#ifdef LORA_PIN_TXEN
    radio_->setRfSwitchPins(LORA_PIN_RXEN, LORA_PIN_TXEN);
#endif
    radio_->setDio1Action(dioIsrTrampoline);

    // Flush any stale FIFO contents and start listening.
    uint8_t scratch[256];
    radio_->readData(scratch, sizeof(scratch));
    radio_->startReceive();
    return true;
}

void RadioSX128x::transmit(const uint8_t* data, size_t len) {
    if (radio_ == nullptr) {
        return;
    }
#ifdef LORA_PIN_ANT
    // Alternate diversity antennas per transmission.
    static uint8_t ant = 0;
    digitalWrite(LORA_PIN_ANT, ant & 1);
    ant++;
#endif
    if (transmitting_) {
        // A previous frame is still on the air. Calling startTransmit() again
        // here would abandon it mid-packet and put a truncated frame on the
        // channel, which is worse than sending nothing. The Node beacons each
        // radio independently and announces fan out to all of them, so two
        // sends CAN land in the same loop iteration -- often enough that it
        // used to show up as a drop count climbing on a radio that was working
        // perfectly.
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

void RadioSX128x::startTx(const uint8_t* data, size_t len) {
    transmitting_ = true;
    tx_start_ms_ = millis();
    // RadioLib writes the payload into the chip's buffer here and returns, so
    // the caller's buffer does not have to outlive this call -- which is what
    // lets sendPending() clear the slot immediately afterwards.
    radio_->startTransmit(const_cast<uint8_t*>(data), len);
}

bool RadioSX128x::sendPending() {
    if (pending_.empty()) {
        return false;
    }
    startTx(pending_.data(), pending_.size());
    pending_.clear();
    return true;
}

void RadioSX128x::serviceRx() {
    if (radio_ == nullptr) {
        return;
    }

    // Anything that could have freed the radio sets this, so the tail of the
    // function knows whether it has to put the radio back to work. Skipping
    // that on a quiet pass matters: startReceive() is an SPI transaction, and
    // this runs every time round the main loop.
    bool radio_freed = false;

    // Watchdog first, and outside the dio_pending_ shortcut: a missed
    // transmit-done interrupt leaves transmitting_ stuck true, every later
    // transmit is then dropped by the guard above, and the radio goes quiet
    // with nothing but the counters climbing to say so.
    if (transmitting_ && (millis() - tx_start_ms_) > kTxTimeoutMs) {
        tx_timeouts_++;
        transmitting_ = false;
        radio_->finishTransmit();
        radio_freed = true;
    }

    if (dio_pending_) {
        dio_pending_ = false;
        radio_freed = true;
        serviceIrq();
    }

    if (!radio_freed || transmitting_) {
        return;
    }
    // The radio is idle and something just finished on it. Send the frame that
    // was held back, if there is one, and only otherwise go back to listening:
    // startReceive() after a startTransmit() aborts the transmission.
    if (!sendPending()) {
        radio_->startReceive();
    }
}

void RadioSX128x::serviceIrq() {
    const uint16_t flags = radio_->getIrqStatus();

    // Both can be set: a packet may have arrived while we were still holding a
    // completed transmit. Handle each, rather than returning after the first.
    if (flags & RADIOLIB_SX128X_IRQ_TX_DONE) {
        radio_->finishTransmit();
        transmitting_ = false;
    }

    if ((flags & RADIOLIB_SX128X_IRQ_RX_DONE) &&
        !(flags & RADIOLIB_SX128X_IRQ_CRC_ERROR)) {
        RxFrame frame{};
        size_t len = radio_->getPacketLength();
        if (len > sizeof(frame.data)) {
            len = sizeof(frame.data);
        }
        int state = radio_->readData(frame.data, len);
        if (state == RADIOLIB_ERR_NONE) {
            frame.timestamp_ms = millis();
            frame.rssi = static_cast<int16_t>(radio_->getRSSI());
            last_snr_db_ = radio_->getSNR();
            have_snr_ = true;
            frame.len = static_cast<uint8_t>(len);
            rx_.push(frame);
        }
    }
}

RadioSX128x::Info RadioSX128x::info() const {
    Info i;
    i.frequency_hz = static_cast<uint32_t>(LORA_FREQUENCY);
    i.bandwidth_khz = kBandwidthKHz;
    i.spreading_factor = kSpreadingFactor;
    i.coding_rate = kCodingRate;
    i.power_dbm = static_cast<int8_t>(LORA_POWER);
    i.has_snr = have_snr_;
    i.last_snr_db = last_snr_db_;
    return i;
}

double RadioSX128x::airtimeMs(size_t payload_len) const {
    LoraParams p{};
    p.spreading_factor = kSpreadingFactor;
    p.bandwidth_hz = static_cast<uint32_t>(kBandwidthKHz * 1000.0f);
    p.coding_rate_denom = kCodingRate;
    p.preamble_symbols = kPreambleSymbols;
    p.explicit_header = true;
    p.crc_on = true;
    p.low_data_rate_optimize = false;
    return loraAirtimeMs(p, payload_len);
}

}  // namespace ff

#endif  // LORA_FAMILY_SX128X
