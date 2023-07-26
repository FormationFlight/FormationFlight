#pragma once

#include "RadioManager.h"
#include <RadioLib.h>

#ifdef LORA_FAMILY_SX128X
//#define FREQUENCY 2500.0f // MHz
#define FREQUENCY (float)LORA_FREQUENCY / 1000000.0
#define BANDWIDTH 406.25 // kHz
#define SPREADING_FACTOR 5 // SF5
#define CODING_RATE 6 // 4/6 CR
#define SYNC_WORD 0x17 // Arbitrarily chosen
#define PREAMBLE_LENGTH 12 // symbols

#define RECEIVE_TIMEOUT LORA_M3_SLOT_SPACING * LORA_M3_NODES - 1
#endif
class LoRa_SX128X : public Radio
{
public:
    LoRa_SX128X();
    static LoRa_SX128X *getSingleton();

    int begin();
    void transmit(air_type0_t *air_0, uint8_t ota_nonce);
    void receive();
    void flagPacketReceived();
    void loop();
    String getStatusString();
    String getCounterString();
private:
    SX1281* radio = nullptr;
    volatile bool packetReceived = false;
};

