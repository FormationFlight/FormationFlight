#pragma once

#include "GNSSManager.h"

class MSP_GNSS : public GNSSProvider {
public:
    MSP_GNSS();
    GNSSLocation getLocation();
    void begin();
    void loop();
};