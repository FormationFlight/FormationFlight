#include "LoRa_SX127X.h"
#include "RadioManager.h"
#include "../CryptoManager.h"

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

void LoRa_SX127X::transmit(air_type0_t *air_0)
{
    if (!getEnabled()) {
        return;
    }
    uint8_t buf[sizeof(air_type0_t)];
    memcpy_P(buf, air_0, sizeof(air_type0_t));
    /*Serial.print("TX(d): ");
    for (uint8_t i = 0; i < sizeof(air_type0_t); i++) {
        Serial.printf("%02X ", buf[i]);
    }
    Serial.println();*/
    CryptoManager::getSingleton()->encrypt(buf, sizeof(air_type0_t));
    /*Serial.print("TX(e): ");
    for (uint8_t i = 0; i < sizeof(air_type0_t); i++) {
        Serial.printf("%02X ", buf[i]);
    }
    Serial.println();*/
    stateTransmitting = true;
    DBGLN("t");
    int16_t result = radio->transmit(buf, sizeof(air_type0_t));
    //if (result != RADIOLIB_ERR_NONE) {
    //    DBGF("transmit error: %d\n", result);
    //}
    stateTransmitting = false;
    packetsTransmitted++;
    lastTransmitTime = millis();
    radio->startReceive(sizeof(air_type0_t));
}

int LoRa_SX127X::begin() {
#ifdef LORA_FAMILY_SX127X
    SPI.begin(LORA_PIN_SCK, LORA_PIN_MISO, LORA_PIN_MOSI, LORA_PIN_CS);
    radio = new SX1276(new Module(LORA_PIN_CS, LORA_PIN_DIO0, LORA_PIN_RST));
    radio->reset();
    //Serial.printf("Radio version: %d\n", radio->getChipVersion());
    int16_t result = radio->begin(FREQUENCY, BANDWIDTH, SPREADING_FACTOR, CODING_RATE, SYNC_WORD, LORA_POWER, PREAMBLE_LENGTH, LNA_GAIN);
    if (result != RADIOLIB_ERR_NONE) {
        Serial.printf("failed to init radio: %d\n", result);
        while (true) {}
    }
    radio->setCRC(0);
    #ifdef LORA_PIN_RXEN
    radio->setRfSwitchPins(LORA_PIN_RXEN, LORA_PIN_TXEN);
    #endif
    radio->setCurrentLimit(0);
    radio->implicitHeader(sizeof(air_type0_t));
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
    if (radio->getIRQFlags() & RADIOLIB_SX127X_CLEAR_IRQ_FLAG_RX_DONE)
    //if (!stateTransmitting) 
        packetReceived = true;
    //radio->startReceive(sizeof(air_type0_t), RADIOLIB_SX127X_RXSINGLE);
}

void LoRa_SX127X::receive()
{
    DBGLN("rx");
    uint8_t buf[sizeof(air_type0_t)];
    radio->readData(buf, sizeof(air_type0_t));
    radio->startReceive(sizeof(air_type0_t));
    /*Serial.print("RX (e): ");
    for (uint8_t i = 0; i < sizeof(air_type0_t); i++) {
        Serial.printf("%02X ", buf[i]);
    }
    Serial.print("\n");*/
    CryptoManager::getSingleton()->decrypt(buf, sizeof(air_type0_t));

    /*Serial.print("RX (d): ");
        for (uint8_t i = 0; i < sizeof(air_type0_t); i++) {
        Serial.printf("%02X ", buf[i]);
    }
    Serial.print("\n");*/
    //radio->startReceive(sizeof(air_type0_t));
    handleReceiveCounters(RadioManager::getSingleton()->receive(buf, sizeof(air_type0_t), radio->getRSSI()));
    
}

void LoRa_SX127X::loop()
{
    if (packetReceived)
    {
        receive();
        packetReceived = false;
    }
}

String LoRa_SX127X::getStatusString()
{
    char buf[128];
#ifdef LORA_FAMILY_SX127X
    sprintf(buf, "LoRa SX127X @ %fMHz (%ddBm) [%uTX/%uRX] [%uCRC/%uSIZE/%uVAL]", FREQUENCY, LORA_POWER, packetsTransmitted, packetsReceived, packetsBadCrc, packetsBadSize, packetsBadValidation);
#endif
    return String(buf);

}