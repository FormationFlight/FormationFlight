#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "MSP.h"
#include "../GNSS/GNSSManager.h"
#include "../Peers/PeerManager.h"

#define HOST_MSP_TIMEOUT 8500

enum MSPHost {
    HOST_NONE = 0,
    HOST_GCS = 1,
    HOST_INAV = 2,
    HOST_ARDU = 3,
    HOST_BTFL = 4
};

class MSPManager {
public:
    MSPManager();
    uint8_t getState();
    void getName(char *name, size_t length);
    MSPHost getFCVariant();
    static bool hostIsFlightController(MSPHost host);
    msp_fc_version_t getFCVersion();
    msp_raw_gps_t getLocation();
    msp_analog_t getAnalogValues();
    void sendRadar(const peer_t *peer);
    void sendLocation(GNSSLocation location);
    void begin(Stream &stream);
    void statusJson(JsonDocument *doc);
    void scheduleNextAt(unsigned long timestamp);
    void loop();

    static MSPManager* getSingleton();
private:
    MSP *msp = nullptr;
    bool ready = false;
    // Counter indicating how many peer updates have been sent over MSP
    uint32_t peerUpdatesSent = 0;
    // Counter indicating how many GPS positions have been injected into the FC via MSP
    uint32_t gnssUpdatesSent = 0;
    // Next timestamp at which we'll transmit a single peer's Radar MSP message
    unsigned long nextSendTime = 0;
    // Which peer we'll send next
    uint8_t peerIndex = 0;

    uint8_t mapFixType2Msp(GNSS_FIX_TYPE fixType);
};