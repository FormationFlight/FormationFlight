#ifdef LORA_FAMILY_SX127X

#include "RadioSX127x.h"

#include <Arduino.h>
#include <SPI.h>

#include "airtime.h"

namespace ff {

// Compile-time band-edge check. The channel, bandwidth included, must sit wholly
// inside its allocation. This exists because the 2.4 GHz default once had half
// its channel below 2400 MHz: the kind of mistake that should never survive a
// build, and that nothing else here would have caught.
namespace {
constexpr double kCentreHz = static_cast<double>(LORA_FREQUENCY);
constexpr double kHalfBwHz = LORA_BW_KHZ * 1000.0 / 2.0;
#if LORA_BAND == 433
constexpr double kBandLowHz = 433.05e6;
constexpr double kBandHighHz = 434.79e6;
#elif LORA_BAND == 868
// The 500 mW / 10% sub-band, which is the only EU one that can carry a useful
// beacon rate. Deliberately NOT the whole 863-870 allocation: the sub-band
// boundaries are where the power and duty limits change.
constexpr double kBandLowHz = 869.40e6;
constexpr double kBandHighHz = 869.65e6;
#elif LORA_BAND == 915
constexpr double kBandLowHz = 902.0e6;
constexpr double kBandHighHz = 928.0e6;
#else
#error "unknown LORA_BAND for the sub-GHz radio"
#endif
static_assert(kCentreHz - kHalfBwHz >= kBandLowHz,
              "LoRa channel extends below the band edge; check LORA_FREQUENCY and LORA_BW_KHZ");
static_assert(kCentreHz + kHalfBwHz <= kBandHighHz,
              "LoRa channel extends above the band edge; check LORA_FREQUENCY and LORA_BW_KHZ");
}  // namespace

namespace {

// Modulation comes from the band definition in platformio.ini, because the
// legal channel width is a property of the band, not of the chip. See the
// comments there for why each one is what it is.
#ifndef LORA_BW_KHZ
#define LORA_BW_KHZ 500.0
#endif
#ifndef LORA_SF
#define LORA_SF 7
#endif
#ifndef LORA_CR
#define LORA_CR 5
#endif
constexpr float kBandwidthKHz = static_cast<float>(LORA_BW_KHZ);
constexpr uint8_t kSpreadingFactor = LORA_SF;
constexpr uint8_t kCodingRate = LORA_CR;
constexpr uint8_t kSyncWord = 0x17;
constexpr uint16_t kPreambleSymbols = 8;
constexpr uint8_t kLnaGain = 0;      // automatic

RadioSX127x* g_instance = nullptr;

void IRAM_ATTR dioIsrTrampoline() {
    if (g_instance != nullptr) {
        g_instance->onDioIsr();
    }
}

}  // namespace

bool RadioSX127x::begin() {
    g_instance = this;

#if defined(PLATFORM_ESP32)
    SPI.begin(LORA_PIN_SCK, LORA_PIN_MISO, LORA_PIN_MOSI, LORA_PIN_CS);
#else
    SPI.begin();
#endif

#if LORA_BAND == 433
    radio_ = new SX1278(new Module(LORA_PIN_CS, LORA_PIN_DIO0, LORA_PIN_RST));
#else
    radio_ = new SX1276(new Module(LORA_PIN_CS, LORA_PIN_DIO0, LORA_PIN_RST));
#endif
    radio_->reset();

    const float freq_mhz = static_cast<float>(LORA_FREQUENCY) / 1000000.0f;
    int state = radio_->begin(freq_mhz, kBandwidthKHz, kSpreadingFactor, kCodingRate,
                              kSyncWord, LORA_POWER, kPreambleSymbols, kLnaGain);
    if (state != RADIOLIB_ERR_NONE) {
        return false;
    }
#ifdef LORA_PIN_RXEN
    radio_->setRfSwitchPins(LORA_PIN_RXEN, LORA_PIN_TXEN);
#endif
    radio_->setCurrentLimit(0);
    radio_->setDio0Action(dioIsrTrampoline);
    radio_->startReceive();  // explicit header => variable length
    return true;
}

void RadioSX127x::transmit(const uint8_t* data, size_t len) {
    if (radio_ == nullptr) {
        return;
    }
    if (transmitting_) {
        // A previous frame is still on the air. Calling startTransmit() again
        // here would abandon it mid-packet and put a truncated frame on the
        // channel, which is worse than sending nothing. The Node beacons each
        // radio independently and announces fan out to all of them, so two
        // sends CAN land in the same loop iteration -- on the 915 settings
        // that happens about once every six seconds, which used to show up as
        // a drop count climbing on a radio that was working perfectly.
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

void RadioSX127x::startTx(const uint8_t* data, size_t len) {
    transmitting_ = true;
    tx_start_ms_ = millis();
    // RadioLib writes the payload into the chip's FIFO here and returns, so the
    // caller's buffer does not have to outlive this call -- which is what lets
    // sendPending() clear the slot immediately afterwards.
    radio_->startTransmit(const_cast<uint8_t*>(data), len);
}

bool RadioSX127x::sendPending() {
    if (pending_.empty()) {
        return false;
    }
    startTx(pending_.data(), pending_.size());
    pending_.clear();
    return true;
}

void RadioSX127x::serviceRx() {
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

void RadioSX127x::serviceIrq() {
    const uint16_t flags = radio_->getIRQFlags();

    // Both can be set: a packet may have arrived while we were still holding a
    // completed transmit. Handle each, rather than returning after the first.
    if (flags & RADIOLIB_SX127X_CLEAR_IRQ_FLAG_TX_DONE) {
        radio_->finishTransmit();
        transmitting_ = false;
    }

    if ((flags & RADIOLIB_SX127X_CLEAR_IRQ_FLAG_RX_DONE) &&
        !(flags & RADIOLIB_SX127X_CLEAR_IRQ_FLAG_PAYLOAD_CRC_ERROR)) {
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

RadioSX127x::Info RadioSX127x::info() const {
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

double RadioSX127x::airtimeMs(size_t payload_len) const {
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

#endif  // LORA_FAMILY_SX127X
