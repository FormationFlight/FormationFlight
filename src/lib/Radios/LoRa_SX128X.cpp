#include "RadioManager.h"
#include "LoRa_SX128X.h"
#include "../Cryptography/CryptoManager.h"

void IRAM_ATTR onSX128XPacketReceive(void)
{
    LoRa_SX128X::getSingleton()->flagPacketReceived();
}

LoRa_SX128X *lora128XInstance = nullptr;

LoRa_SX128X::LoRa_SX128X()
{

}

LoRa_SX128X* LoRa_SX128X::getSingleton()
{
    if (lora128XInstance == nullptr)
    {
        lora128XInstance = new LoRa_SX128X();
    }
    return lora128XInstance;
}

void LoRa_SX128X::transmit(air_type0_t *air_0, uint8_t ota_nonce)
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
        DBGF("[SX128X]: TX Status %d\n", state);
    }
    packetsTransmitted++;
}

int LoRa_SX128X::begin() {
#ifdef LORA_FAMILY_SX128X
#ifdef PLATFORM_ESP32
    SPI.begin(LORA_PIN_SCK, LORA_PIN_MISO, LORA_PIN_MOSI, LORA_PIN_CS);
#else
    SPI.begin();
#endif
    radio = new SX1281(new Module(LORA_PIN_CS, LORA_PIN_DIO, LORA_PIN_RST, LORA_PIN_BUSY));
    radio->begin(FREQUENCY, BANDWIDTH, SPREADING_FACTOR, CODING_RATE, SYNC_WORD, LORA_POWER, PREAMBLE_LENGTH);
    // We appear to need to set this twice
    radio->setOutputPower(LORA_POWER);
    //radio->setCRC(0);
    #ifdef LORA_PIN_TXEN
    radio->setRfSwitchPins(LORA_PIN_RXEN, LORA_PIN_TXEN);
    #endif
    //radio->implicitHeader(sizeof(air_type0_t));
    radio->setDio1Action(onSX128XPacketReceive);
    radio->startReceive();

#endif
    return 0;
}

void LoRa_SX128X::flagPacketReceived()
{
    if (!getEnabled()) {
        return;
    }
    packetReceived = true;
}

void LoRa_SX128X::receive()
{
    uint8_t buf[sizeof(air_type0_t)];
    memset(buf, 0, sizeof(buf));
    int state = radio->readData(buf, sizeof(air_type0_t));
    if (state != RADIOLIB_ERR_NONE) {
        DBGF("[SX1280] readData result: %d\n", state);
        return;
    }
    radio->startReceive();
    CryptoManager::getSingleton()->decrypt(buf, sizeof(air_type0_t));
    handleReceiveCounters(RadioManager::getSingleton()->receive(buf, sizeof(air_type0_t), radio->getRSSI()));
}

void LoRa_SX128X::loop()
{
    if (packetReceived)
    {
        uint16_t flags = radio->getIrqStatus();
        if (flags == RADIOLIB_SX128X_IRQ_RX_DONE) {
            receive();
        }
        if (flags & RADIOLIB_SX128X_IRQ_TX_DONE) {
            sys.last_tx_end = millis();
            radio->finishTransmit();
            // We need to clear IRQ flags and start a new RX cycle
            radio->startReceive();
        }
    }
    packetReceived = false;
}

String LoRa_SX128X::getStatusString()
{
    char buf[128];
#ifdef LORA_FAMILY_SX128X
    sprintf(buf, "LoRa SX128X @ %.02fMHz (%ddBm)", FREQUENCY, LORA_POWER);
#endif
    return String(buf);
}


String LoRa_SX128X::getCounterString()
{
    char buf[128];
#ifdef LORA_FAMILY_SX128X
    sprintf(buf, "[%uTX/%uRX] [%uCRC/%uSIZE/%uVAL]", packetsTransmitted, packetsReceived, packetsBadCrc, packetsBadSize, packetsBadValidation);
#endif
    return String(buf);
}