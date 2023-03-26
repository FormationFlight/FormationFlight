#include "WiFiManager.h"
#include "../Helpers.h"
#ifdef PLATFORM_ESP8266
#include <ESP8266WiFi.h>
#elif defined(PLATFORM_ESP32)
#include <WiFi.h>
#endif
// OTA
#include <ArduinoOTA.h>

WiFiManager::WiFiManager()
{
    // Setup WiFi network
#ifdef PLATFORM_ESP32
    WiFi.mode(WIFI_MODE_AP);
#elif defined(PLATFORM_ESP8266)
    WiFi.mode(WIFI_AP);
#endif
    String chipIDString = generate_id();
    String ssid = "iNav Radar-";
    ssid += chipIDString;
    WiFi.softAP(ssid.c_str(), "inavradar");
    server = new AsyncWebServer(80);
    server->on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", "OK");
    });
    server->on("/peers", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", String(peers[0].id));
    });
    server->begin();
    // Setup OTA updates
    ArduinoOTA.begin();
}

WiFiManager *wifiManager = nullptr;

WiFiManager* WiFiManager::getSingleton()
{
    if (wifiManager == nullptr)
    {
        wifiManager = new WiFiManager();
    }
    return wifiManager;
}

void WiFiManager::loop()
{
    // OTA update loop
    ArduinoOTA.handle();
}