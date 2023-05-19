#pragma once

#include <Arduino.h>
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
    static bool hostTXCapable(MSPHost host);
    msp_fc_version_t getFCVersion();
    msp_raw_gps_t getLocation();
    msp_analog_t getAnalogValues();
    void sendRadar(peer_t *peer);
    void sendLocation(GNSSLocation location);
    static MSPManager* getSingleton();
    void begin(Stream &stream);
private:
    MSP *msp = nullptr;
    bool ready = false;
};