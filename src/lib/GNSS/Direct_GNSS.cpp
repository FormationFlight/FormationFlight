#include "Direct_GNSS.h"
#include "../MSP/MSPManager.h"
#include "main.h"


// Set GPS back to defaults
void gpsSetDefaults(HardwareSerial *serial, int8_t pinRx, int8_t pinTx)
{
    const uint8_t resetCommand[] = { 0xB5, 0x62, 0x06, 0x09, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x03, 0x1D, 0xB3 };
    const uint32_t bauds[] = { 921600, 115200, 57600, 38400, 19200, 9600, 4800 };
    for (uint8_t i = 0; i < sizeof(bauds) / sizeof(bauds[0]); i++)
    {
        serial->begin(bauds[i], SERIAL_8N1, pinRx, pinTx);
        serial->write(resetCommand, sizeof(resetCommand) / sizeof(resetCommand[0]));
    }
}

void detectBaud(HardwareSerial *serial, int8_t pinRx, int8_t pinTx, uint8_t attempts = 0)
{
    const size_t bufLength = 128;
    char buf[bufLength];
    const uint32_t bauds[] = { 921600, 115200, 57600, 38400, 19200, 9600, 4800 };
    for (uint8_t i = 0; i < sizeof(bauds) / sizeof(bauds[0]); i++)
    {
        DBGF("[Direct_GNSS] Autobaud trying %d\n", bauds[i]);
        serial->flush();
        serial->begin(bauds[i], SERIAL_8N1, pinRx, pinTx);
        // Flush the buffer
        while (Serial.available()) {
            Serial.read();
        }
        serial->readBytes(buf, bufLength);
        buf[bufLength - 1] = '\0';
        if (strstr(buf, "$G")) {
            DBGF("[Direct_GNSS] Autobaud found %d\n", bauds[i]);
            return;
        }
    }
    if (attempts == 0) {
        DBGF("[Direct_GNSS] Autobaud failed, attempting fix\n");
        gpsSetDefaults(serial, pinRx, pinTx);
        detectBaud(serial, pinTx, pinRx, 1);
    } else {
        DBGF("[Direct_GNSS] Autobaud failed, direct GNSS on-device will be unavailable\n");
        // Last ditch
        serial->begin(9600, SERIAL_8N1, pinRx, pinTx);
    }
    

}

Direct_GNSS::Direct_GNSS()
{
    gps = new TinyGPSPlus();
#ifdef GNSS_ENABLED
    gpsSerial = new HardwareSerial(GNSS_UART_INDEX);
    detectBaud(gpsSerial, GNSS_PIN_RX, GNSS_PIN_TX);
    //fix();
    //gpsSerial->updateBaudRate(GNSS_BAUD);
    stream = gpsSerial;
#endif
}

void Direct_GNSS::loop()
{
    for (uint8_t i = 0; i < min(8, stream->available()); i++) {
        char c = stream->read();
        gps->encode(c);
        if (c == '\n') {
            linesProcessed++;
        }
        //Serial.print(String(c));
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
    sprintf(buf, "DIRECT GNSS @ UART%d [%dSAT/%uUPD] (%f,%f) (%fm)", GNSS_UART_INDEX, gps->satellites.value(), linesProcessed, loc.lat, loc.lon, loc.alt);
#endif
    return String(buf);
}

String Direct_GNSS::getName()
{
    return String("DIR");
}
