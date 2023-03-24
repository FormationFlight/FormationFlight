#pragma once
#include <main.h>

#define MAX_RADIOS 4

enum RadioState
{
    RADIO_STATE_DISABLED,
    RADIO_STATE_INIT,
    RADIO_STATE_RX,
    RADIO_STATE_TX
};

class Radio
{
public:
    virtual int begin();
    virtual void transmit(air_type0_t *packet);
    virtual void loop();
};

class RadioManager
{
public:
    RadioManager();
    static RadioManager *getSingleton();

    air_type0_t prepare_packet();
    void receive(const uint8_t *rawPacket, size_t packetSize, float rssi);
    void transmit(air_type0_t *packet);

    void addRadio(Radio *radio);
    void loop();
private:
    Radio *radios[MAX_RADIOS] = {nullptr};
};