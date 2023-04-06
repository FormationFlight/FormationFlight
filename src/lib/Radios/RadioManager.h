#pragma once
#include <main.h>
#include <ArduinoJson.h>

#define MAX_RADIOS 4

enum PACKETTYPE {
    // Starting at 1 such that an all-zeros packet (with valid CRC) is invalid.
    // In burn-in testing, I saw a handful of these come through during multi-hour runs.
    PACKET_TYPE_RADAR_POSITION = 1
};

enum RadioState
{
    RADIO_STATE_DISABLED,
    RADIO_STATE_INIT,
    RADIO_STATE_RX,
    RADIO_STATE_TX
};

enum ReceiveResult {
    RECEIVE_RESULT_OK,
    RECEIVE_RESULT_BAD_SIZE,
    RECEIVE_RESULT_BAD_PACKET_TYPE,
    RECEIVE_RESULT_BAD_CRC,
    RECEIVE_RESULT_BAD_ID,
    RECEIVE_RESULT_BAD_FIELD,
};

class Radio
{
public:
    virtual int begin();
    virtual void transmit(air_type0_t *packet, uint8_t ota_nonce);
    virtual void loop();
    virtual String getStatusString();
    bool getEnabled() {
        return enabled;
    }
    void setEnabled(bool status) {
        enabled = status;
    }
    void handleReceiveCounters(ReceiveResult result) {
        switch (result) {
            case RECEIVE_RESULT_BAD_CRC:
            packetsBadCrc++;
            break;
            case RECEIVE_RESULT_BAD_SIZE:
            packetsBadSize++;
            break;
            case RECEIVE_RESULT_BAD_ID:
            case RECEIVE_RESULT_BAD_PACKET_TYPE:
            case RECEIVE_RESULT_BAD_FIELD:
            packetsBadValidation++;
            break;
            case RECEIVE_RESULT_OK:
            default:
            packetsReceived++;
            break;
        }
    }
protected:
    uint32_t packetsReceived = 0;
    uint32_t packetsTransmitted = 0;
    uint32_t packetsBadCrc = 0;
    uint32_t packetsBadSize = 0;
    uint32_t packetsBadValidation = 0;
    bool enabled = true;
    uint32_t lastTransmitTime;
};

class RadioManager
{
public:
    RadioManager();
    static RadioManager *getSingleton();

    air_type0_t prepare_packet();
    ReceiveResult receive(const uint8_t *rawPacket, size_t packetSize, float rssi);
    void transmit(air_type0_t *packet, uint8_t ota_nonce);

    void addRadio(Radio *radio);
    void loop();
    void statusJson(JsonDocument *doc);
    void setRadioStatus(uint8_t index, bool status);
private:
    Radio *radios[MAX_RADIOS] = {nullptr};
};