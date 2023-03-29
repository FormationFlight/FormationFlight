#pragma once

#include <Arduino.h>
#include "GNSSManager.h"
#include <TinyGPSPlus.h>

#define UPDATE_LOOP_DELAY_MS 100

// A directly-attached GNSS chip, such as is present on the TTGO T-Beam
class Direct_GNSS : public GNSSProvider {
public:
    Direct_GNSS();
    GNSSLocation getLocation();
    String getStatusString();
    void loop();
private:
    TinyGPSPlus *gps;
    Stream *stream;
    unsigned long lastUpdate = 0;
    uint32_t bytesProcessed = 0;
};