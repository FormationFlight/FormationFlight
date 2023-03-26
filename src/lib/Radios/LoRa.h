#include "RadioManager.h"
#include <RadioLib.h>

#define FREQUENCY 2500.0f // MHz
#define BANDWIDTH 203 // kHz
#define SPREADING_FACTOR 5 // SF5
#define CODING_RATE 6 // 4/6 CR
#define SYNC_WORD 0x17 // Arbitrarily chosen
#define PREAMBLE_LENGTH 12

#define RECEIVE_TIMEOUT LORA_M3_SLOT_SPACING * LORA_M3_NODES - 1

class LoRa : public Radio
{
public:
    LoRa();
    static LoRa *getSingleton();

    int begin();
    void transmit(air_type0_t *air_0);
    void receive();
    void flagPacketReceived();
    void loop();
    String getStatusString();
private:
    SX128x radio = nullptr;
    volatile bool packetReceived;
};