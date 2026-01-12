#include "RadioManager.h"
#include "LoRa_SX126X.h"
#include "../Cryptography/CryptoManager.h"

void IRAM_ATTR onSX126XPacketReceive(void)
{
    LoRa_SX126X::getSingleton()->flagPacketReceived();
}

LoRa_SX126X *lora126XInstance = nullptr;

LoRa_SX126X::LoRa_SX126X()
{

}

LoRa_SX126X* LoRa_SX126X::getSingleton()
{
    if (lora126XInstance == nullptr)
    {
        lora126XInstance = new LoRa_SX126X();
    }
    return lora126XInstance;
}

void LoRa_SX126X::transmit(air_type0_t *air_0, uint8_t ota_nonce)
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
        DBGF("[SX126X]: TX Status %d\n", state);
    }
    packetsTransmitted++;
}

int LoRa_SX126X::begin() {
#ifdef LORA_FAMILY_SX126X
#ifdef PLATFORM_ESP32
    SPI.begin(LORA_PIN_SCK, LORA_PIN_MISO, LORA_PIN_MOSI);
#else
    SPI.begin();
#endif

    radio = new SX1262(new Module(LORA_PIN_NSS, LORA_PIN_DIO1, LORA_PIN_RST, LORA_PIN_BUSY));

    radio->reset(false);
    //Serial.printf("Radio version: %d\n", radio->getChipVersion());
    int16_t result = radio->begin(FREQUENCY, BANDWIDTH, SPREADING_FACTOR, CODING_RATE, SYNC_WORD, PREAMBLE_LENGTH);
    if (result != RADIOLIB_ERR_NONE) {
        Serial.printf("[SX126X]: failed to init radio: %d\n", result);
        while (true) {}
    }
    radio->setOutputPower(LORA_POWER);
    //radio->setCRC(0);
    #ifdef LORA_PIN_RXEN
    radio->setRfSwitchPins(LORA_PIN_RXEN, LORA_PIN_TXEN);
    #endif
    //radio->implicitHeader(sizeof(air_type0_t));
    radio->setDio1Action(onSX126XPacketReceive);
    uint8_t buf[256];
    radio->readData(buf, sizeof(buf));
    radio->startReceive();
#endif
    return 0;
}

void LoRa_SX126X::flagPacketReceived()
{
    if (!getEnabled()) {
        return;
    }
    packetReceived = true;

}

void LoRa_SX126X::receive()
{
    uint8_t buf[sizeof(air_type0_t)];
    memset(buf, 0, sizeof(buf));
    int state = radio->readData(buf, sizeof(air_type0_t));
    if (state != RADIOLIB_ERR_NONE) {
        DBGF("[SX126X] readData result: %d\n", state);
        return;
    }
    radio->startReceive();
    CryptoManager::getSingleton()->decrypt(buf, sizeof(air_type0_t));
    handleReceiveCounters(RadioManager::getSingleton()->receive(buf, sizeof(air_type0_t), radio->getRSSI()));
    
}

void LoRa_SX126X::loop()
{
    if (packetReceived) {
        uint16_t flags = radio->getIrqStatus();
        if (flags & RADIOLIB_SX126X_IRQ_RX_DONE || flags == RADIOLIB_SX126X_IRQ_TIMEOUT)
        {
            receive();
        }
        if (flags & RADIOLIB_SX126X_IRQ_TX_DONE) {
            sys.last_tx_end = millis();
            radio->finishTransmit();
            // We need to clear IRQ flags and start a new RX cycle
            radio->startReceive();
        }
    }
    packetReceived = false;
}

String LoRa_SX126X::getStatusString()
{
    char buf[128];
#ifdef LORA_FAMILY_SX126X
    sprintf(buf, "LoRa SX126X @ %.02fMHz (%ddBm)", FREQUENCY, LORA_POWER);
#endif
    return String(buf);
}



String LoRa_SX126X::getCounterString()
{
    char buf[128];
#ifdef LORA_FAMILY_SX126X
    sprintf(buf, "[%uTX/%uRX] [%uCRC/%uSIZE/%uVAL]", packetsTransmitted, packetsReceived, packetsBadCrc, packetsBadSize, packetsBadValidation);
#endif
    return String(buf);
}