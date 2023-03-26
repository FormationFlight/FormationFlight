#ifdef WIFI_CONFIG
#include <Arduino.h>
#include "WiFiConfig.h"
#ifdef PLATFORM_ESP8266
#include "ESP8266WiFi.h"
#elif defined(PLATFORM_ESP32)
#include "esp_wifi.h"
#endif
#include "ESPTelnet/ESPTelnetStream.h"

ESPTelnetStream telnet;

void setupNetwork()
{
    uint32_t chipID;
#ifdef PLATFORM_ESP8266
    chipID = ESP.getChipId();
#elif defined(PLATFORM_ESP32)
    uint64_t macAddress = ESP.getEfuseMac();
    uint64_t macAddressTrunc = macAddress << 40;
    chipID = macAddressTrunc >> 40;
#endif
    String chipIDString = String(chipID, HEX);
    String ssid = "Radar-";
    ssid += chipIDString;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid.c_str(), "inavradar");
}

void initConfigInterface()
{
    setupNetwork();
    telnet.begin();
}

void handleConfig()
{
    telnet.loop();
    char incoming;
    static String message;
    if (telnet.available())
    {
        incoming = telnet.read();

        if (incoming != '\n')
        {
            message += String(incoming);
        }
        else
        {
            handleConfigMessage(telnet, message);
            message = "";
        }
    }
}

void debugPrintln(String input)
{
    telnet.println(input);
}
#endif