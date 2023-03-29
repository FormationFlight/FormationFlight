#include "Direct_GNSS.h"
#include "../MSP/MSPManager.h"

Direct_GNSS::Direct_GNSS()
{
    gps = new TinyGPSPlus();
#ifdef GNSS_ENABLED
    HardwareSerial *gpsSerial = new HardwareSerial(GNSS_UART_INDEX);
    gpsSerial->begin(GNSS_BAUD, SERIAL_8N1, GNSS_PIN_RX, GNSS_PIN_TX);
    stream = gpsSerial;
#endif
}

void Direct_GNSS::loop()
{
    while (stream->available()) {
        gps->encode(stream->read());
        bytesProcessed++;
    }
    if (gps->location.isValid()) {
        lastUpdate = millis();
    }
}

GNSSLocation Direct_GNSS::getLocation()
{
    GNSSLocation loc;
    loc.alt = gps->altitude.meters();
    loc.fixType = gps->location.isValid() ? gps->altitude.isValid() ? GNSS_FIX_TYPE_3D : GNSS_FIX_TYPE_2D : GNSS_FIX_TYPE_NONE;
    loc.groundCourse = gps->course.deg();
    loc.groundSpeed = gps->speed.kmph();
    loc.hdop = gps->hdop.hdop();
    loc.lastUpdate = lastUpdate;
    loc.lat = gps->location.lat();
    loc.lon = gps->location.lng();
    loc.numSat = gps->satellites.value();
    return loc;
}

String Direct_GNSS::getStatusString()
{
    GNSSLocation loc = getLocation();
    char buf[128];
#ifdef GNSS_ENABLED
    sprintf(buf, "Direct GNSS @ UART%d [%ubytes/%dSAT] (%f,%f) (%fm)", GNSS_UART_INDEX, bytesProcessed, gps->satellites.value(), loc.lat, loc.lon, loc.alt);
#endif
    return String(buf);

}