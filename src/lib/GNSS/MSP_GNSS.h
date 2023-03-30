#pragma once

#include "GNSSManager.h"
#include "../MSP/MSP.h"

#define UPDATE_LOOP_DELAY_MS 100

// A MSP-attached FC with the capability to feed us GNSS readings
class MSP_GNSS : public GNSSProvider, public GNSSListener {
public:
    MSP_GNSS();
    GNSSLocation getLocation();
    String getStatusString();
    void loop();
    void update(GNSSLocation location);
    String getName();
private:
    msp_raw_gps_t rawLocation;
    unsigned long lastUpdate = 0;
    uint32_t locationUpdates = 0;
};