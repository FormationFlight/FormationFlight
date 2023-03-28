#include "LoRa_SX128X.h"
#include "RadioManager.h"
#include "../CryptoManager.h"

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

void LoRa_SX128X::transmit(air_type0_t *air_0)
{
    if (!getEnabled()) {
        return;
    }
    uint8_t buf[sizeof(air_type0_t)];
    memcpy_P(buf, air_0, sizeof(air_type0_t));
    CryptoManager::getSingleton()->encrypt(buf, sizeof(air_type0_t));
    radio->transmit(buf, sizeof(air_type0_t));
    packetsTransmitted++;
    lastTransmitTime = millis();
    radio->startReceive();
}

int LoRa_SX128X::begin() {
#ifdef LORA_FAMILY_SX128X
    SPI.begin(LORA_PIN_SCK, LORA_PIN_MISO, LORA_PIN_MOSI, LORA_PIN_CS);
    radio = new SX128x(new Module(LORA_PIN_CS, LORA_PIN_DIO0, LORA_PIN_RST));
    radio->begin(FREQUENCY, BANDWIDTH, SPREADING_FACTOR, CODING_RATE, SYNC_WORD, LORA_POWER, PREAMBLE_LENGTH);
    radio->setCRC(0);
    #ifdef LORA_PIN_RXEN
    radio->setRfSwitchPins(LORA_PIN_RXEN, LORA_PIN_TXEN);
    #endif
    radio->implicitHeader(sizeof(air_type0_t));
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
    radio->readData(buf, sizeof(air_type0_t));
    CryptoManager::getSingleton()->decrypt(buf, sizeof(air_type0_t));
    handleReceiveCounters(RadioManager::getSingleton()->receive(buf, sizeof(air_type0_t), radio->getRSSI()));
}

void LoRa_SX128X::loop()
{
    if (packetReceived)
    {
        receive();
        packetReceived = false;
    }
}

String LoRa_SX128X::getStatusString()
{
    char buf[128];
#ifdef LORA_FAMILY_SX128X
    sprintf(buf, "LoRa SX128X @ %fMHz (%ddBm) [%uTX/%uRX] [%uCRC/%uSIZE/%uVAL]", FREQUENCY, LORA_POWER, packetsTransmitted, packetsReceived, packetsBadCrc, packetsBadSize, packetsBadValidation);
#endif
    return String(buf);

}