#ifdef LORA_FAMILY_SX128X

#include "RadioSX128x.h"

#include <Arduino.h>
#include <SPI.h>

#include "airtime.h"
#include "protocol.h"

namespace ff {

namespace {

// "M3" modulation, matching the legacy 2.4 GHz profile.
constexpr float kBandwidthKHz = 406.25f;
constexpr uint8_t kSpreadingFactor = 5;
constexpr uint8_t kCodingRate = 6;    // 4/6
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
        // here would abandon it mid-packet and put garbage on the channel. The
        // Node beacons each radio independently and announces fan out to all of
        // them, so two sends CAN land in the same loop iteration; ALOHA is built
        // to tolerate a dropped beacon, a corrupted one it is not.
        tx_dropped_++;
        return;
    }
    transmitting_ = true;
    tx_start_ms_ = millis();
    radio_->startTransmit(const_cast<uint8_t*>(data), len);
}

void RadioSX128x::serviceRx() {
    if (radio_ == nullptr) {
        return;
    }

    // Watchdog first, and outside the dio_pending_ shortcut: a missed
    // transmit-done interrupt leaves transmitting_ stuck true, every later
    // transmit is then dropped by the guard above, and the radio goes quiet with
    // nothing but txDropped() climbing to say so.
    if (transmitting_ && (millis() - tx_start_ms_) > kTxTimeoutMs) {
        tx_timeouts_++;
        transmitting_ = false;
        radio_->finishTransmit();
        radio_->startReceive();
    }

    if (!dio_pending_) {
        return;
    }
    dio_pending_ = false;

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
            frame.len = static_cast<uint8_t>(len);
            rx_.push(frame);
        }
    }

    // Any terminal event returns us to a clean receive.
    radio_->startReceive();
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
