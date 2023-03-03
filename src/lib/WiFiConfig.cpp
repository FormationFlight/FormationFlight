#ifdef WIFI_CONFIG
#include <Arduino.h>
#include "WiFiConfig.h"
#include "ESP8266WiFi.h"
#include "ESPTelnet/ESPTelnetStream.h"

ESPTelnetStream telnet;

void setupNetwork()
{
    String ssid = "iNav Radar-";
    ssid += String(ESP.getChipId(), HEX);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid, "inavradar");
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