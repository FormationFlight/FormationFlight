#pragma once
#include <main.h>
#include <ArduinoJson.h>

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
    virtual String getStatusString();
    bool getEnabled() {
        return enabled;
    }
    void setEnabled(bool status) {
        enabled = status;
    }
private:
    bool enabled;
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
    void statusJson(JsonDocument *doc);
    void setRadioStatus(uint8_t index, bool status);
private:
    Radio *radios[MAX_RADIOS] = {nullptr};
};