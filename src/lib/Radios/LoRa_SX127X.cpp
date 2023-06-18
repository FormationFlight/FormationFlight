#include "LoRa_SX127X.h"
#include "RadioManager.h"
#include "../Cryptography/CryptoManager.h"

void IRAM_ATTR onSX127XPacketReceive(void)
{
    LoRa_SX127X::getSingleton()->flagPacketReceived();
}

LoRa_SX127X *lora127XInstance = nullptr;

LoRa_SX127X::LoRa_SX127X()
{

}

LoRa_SX127X* LoRa_SX127X::getSingleton()
{
    if (lora127XInstance == nullptr)
    {
        lora127XInstance = new LoRa_SX127X();
    }
    return lora127XInstance;
}

void LoRa_SX127X::transmit(air_type0_t *air_0, uint8_t ota_nonce)
{
    if (!getEnabled()) {
        return;
    }
#ifdef LORA_PIN_ANT
    digitalWrite(LORA_PIN_ANT, ota_nonce % 2);
#endif
    uint8_t buf[sizeof(air_type0_t)];
    memcpy_P(buf, air_0, sizeof(air_type0_t));
    CryptoManager::getSingleton()->encrypt(buf, sizeof(air_type0_t));
    int state = radio->startTransmit(buf, sizeof(air_type0_t));
    if (state != RADIOLIB_ERR_NONE) {
        DBGF("[SX127X]: TX Status %d\n", state);
    }
    packetsTransmitted++;
}

int LoRa_SX127X::begin() {
#ifdef LORA_FAMILY_SX127X
#ifdef PLATFORM_ESP32
    SPI.begin(LORA_PIN_SCK, LORA_PIN_MISO, LORA_PIN_MOSI, LORA_PIN_CS);
#else
    SPI.begin();
#endif
#if LORA_BAND==915 || LORA_BAND==868
    radio = new SX1276(new Module(LORA_PIN_CS, LORA_PIN_DIO0, LORA_PIN_RST));
#elif LORA_BAND==433
    radio = new SX1278(new Module(LORA_PIN_CS, LORA_PIN_DIO0, LORA_PIN_RST));
#endif
    radio->reset();
    //Serial.printf("Radio version: %d\n", radio->getChipVersion());
    int16_t result = radio->begin(FREQUENCY, BANDWIDTH, SPREADING_FACTOR, CODING_RATE, SYNC_WORD, LORA_POWER, PREAMBLE_LENGTH, LNA_GAIN);
    if (result != RADIOLIB_ERR_NONE) {
        Serial.printf("failed to init radio: %d\n", result);
        while (true) {}
    }
    //radio->setCRC(0);
    #ifdef LORA_PIN_RXEN
    radio->setRfSwitchPins(LORA_PIN_RXEN, LORA_PIN_TXEN);
    #endif
    radio->setCurrentLimit(0);
    //radio->implicitHeader(sizeof(air_type0_t));
    radio->setDio0Action(onSX127XPacketReceive);
    radio->startReceive(sizeof(air_type0_t));
#endif
    return 0;
}

void LoRa_SX127X::flagPacketReceived()
{
    if (!getEnabled()) {
        return;
    }
    packetReceived = true;

}

void LoRa_SX127X::receive()
{
    uint8_t buf[sizeof(air_type0_t)];
    radio->readData(buf, sizeof(air_type0_t));
    radio->startReceive(sizeof(air_type0_t));
    CryptoManager::getSingleton()->decrypt(buf, sizeof(air_type0_t));
    handleReceiveCounters(RadioManager::getSingleton()->receive(buf, sizeof(air_type0_t), radio->getRSSI()));
    
}

void LoRa_SX127X::loop()
{
    if (packetReceived) {
        uint16_t flags = radio->getIRQFlags();
        if (flags & RADIOLIB_SX127X_CLEAR_IRQ_FLAG_RX_DONE)
        {
            receive();
        }
        if (flags & RADIOLIB_SX127X_CLEAR_IRQ_FLAG_TX_DONE) {
            sys.last_tx_end = millis();
            radio->finishTransmit();
            // We need to clear IRQ flags and start a new RX cycle
            radio->startReceive(sizeof(air_type0_t));
        }
    }
    packetReceived = false;
}

String LoRa_SX127X::getStatusString()
{
    char buf[128];
#ifdef LORA_FAMILY_SX127X
    sprintf(buf, "LoRa SX127X @ %.02fMHz (%ddBm)", FREQUENCY, LORA_POWER);
#endif
    return String(buf);
}



String LoRa_SX127X::getCounterString()
{
    char buf[128];
#ifdef LORA_FAMILY_SX127X
    sprintf(buf, "[%uTX/%uRX] [%uCRC/%uSIZE/%uVAL]", packetsTransmitted, packetsReceived, packetsBadCrc, packetsBadSize, packetsBadValidation);
#endif
    return String(buf);
}