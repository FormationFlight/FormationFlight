#ifdef LORA_FAMILY_SX127X

#include "RadioSX127x.h"

#include <Arduino.h>
#include <SPI.h>

#include "airtime.h"

namespace ff {

namespace {

// "M3" sub-GHz profile, matching the legacy SX127x driver.
constexpr float kBandwidthKHz = 500.0f;
constexpr uint8_t kSpreadingFactor = 7;
constexpr uint8_t kCodingRate = 5;   // 4/5
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
    transmitting_ = true;
    radio_->startTransmit(const_cast<uint8_t*>(data), len);
}

void RadioSX127x::serviceRx() {
    if (radio_ == nullptr || !dio_pending_) {
        return;
    }
    dio_pending_ = false;

    const uint16_t flags = radio_->getIRQFlags();

    if (flags & RADIOLIB_SX127X_CLEAR_IRQ_FLAG_TX_DONE) {
        radio_->finishTransmit();
        transmitting_ = false;
        radio_->startReceive();
        return;
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
            frame.len = static_cast<uint8_t>(len);
            rx_.push(frame);
        }
    }

    radio_->startReceive();
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
