#pragma once

#include "RadioManager.h"
#include <RadioLib.h>
#ifdef LORA_FAMILY_SX127X
#define FREQUENCY (float)LORA_FREQUENCY / 1000000.0
#define BANDWIDTH 500 // kHz
#define SPREADING_FACTOR 7 // SF6
#define CODING_RATE 5 // 4/5 CR
#define SYNC_WORD 0x17 // Arbitrarily chosen
#define PREAMBLE_LENGTH 8 // symbols
#define LNA_GAIN 0 // Automatic gain

#define RECEIVE_TIMEOUT LORA_M3_SLOT_SPACING * LORA_M3_NODES - 1
#endif
class LoRa_SX127X : public Radio
{
public:
    LoRa_SX127X();
    static LoRa_SX127X *getSingleton();

    int begin();
    void transmit(air_type0_t *air_0, uint8_t ota_nonce);
    void receive();
    void flagPacketReceived();
    void loop();
    String getStatusString();
    String getCounterString();
private:
#if LORA_BAND==915 || LORA_BAND==868
    SX1276* radio = nullptr;
#elif LORA_BAND==433
    SX1278* radio = nullptr;
#else
    // Just to make this compile
    SX1276* radio = nullptr;
#endif
    volatile bool packetReceived = false;
};
