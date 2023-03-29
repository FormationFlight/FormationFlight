#include "MSP_GNSS.h"
#include "../MSP/MSPManager.h"

MSP_GNSS::MSP_GNSS()
{
    memset(&rawLocation, 0, sizeof(rawLocation));
}

void MSP_GNSS::loop()
{
    if (millis() - lastUpdate < UPDATE_LOOP_DELAY_MS) {
        return;
    }
    rawLocation = MSPManager::getSingleton()->getLocation();
    if (rawLocation.fixType != GNSS_FIX_TYPE_NONE) {
        locationUpdates++;
    }
    lastUpdate = millis();
}

void MSP_GNSS::update(GNSSLocation loc)
{
    MSPManager::getSingleton()->sendLocation(loc);
}

GNSSLocation MSP_GNSS::getLocation()
{
    GNSSLocation loc;
    loc.alt = rawLocation.alt;
    loc.fixType = (GNSS_FIX_TYPE)rawLocation.fixType;
    loc.groundCourse = rawLocation.groundCourse / 10;
    const double cms_to_kmh = 0.036;
    loc.groundSpeed = rawLocation.groundSpeed * cms_to_kmh;
    loc.hdop = rawLocation.hdop;
    loc.lastUpdate = lastUpdate;
    loc.lat = rawLocation.lat * 10000000;
    loc.lon = rawLocation.lon * 10000000;
    loc.numSat = rawLocation.numSat;
    return loc;
}

String MSP_GNSS::getStatusString()
{
    GNSSLocation loc = getLocation();
    char buf[128];
    sprintf(buf, "MSP GNSS [%dSAT/%uUPD] (%f,%f) (%fm)", loc.numSat, locationUpdates, loc.lat, loc.lon, loc.alt);
    return String(buf);

}