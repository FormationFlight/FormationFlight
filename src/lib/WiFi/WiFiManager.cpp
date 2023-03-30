#include "WiFiManager.h"
#include "../Helpers.h"
#ifdef PLATFORM_ESP8266
#include <ESP8266WiFi.h>
#elif defined(PLATFORM_ESP32)
#include <WiFi.h>
#endif
// OTA
#include <ArduinoOTA.h>
// Config methods
#include <ArduinoJson.h>
#include "../Radios/RadioManager.h"
#include "../Peers/PeerManager.h"
#include "../GNSS/GNSSManager.h"
#include "../Power/PowerManager.h"
#include "../Statistics/StatsManager.h"

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
    server->on("/system/shutdown", HTTP_POST, [](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", "OK");
        ESP.deepSleep(UINT32_MAX);
    });
    // RadioManager
    server->on("/radiomanager/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        StaticJsonDocument<512> doc;
        RadioManager::getSingleton()->statusJson(&doc);
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        serializeJson(doc, *response);
        request->send(response);
    });
    server->on("/radiomanager/radio_set_enabled", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!request->hasParam("index", true) || !request->hasParam("status", true)) {
            request->send(400, "text/plain", "need parameters index & status");
            return;
        }
        long index = request->getParam("index", true)->value().toInt();
        bool status = request->getParam("status", true)->value().equals("true") ? true : false;
        RadioManager::getSingleton()->setRadioStatus(index, status);
        request->send(200, "text/plain", "OK");
    });
    // PeerManager
    server->on("/peermanager/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        StaticJsonDocument<1024> doc;
        PeerManager::getSingleton()->statusJson(&doc);
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        serializeJson(doc, *response);
        request->send(response);
    });
    // GNSSManager
    server->on("/gnssmanager/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        StaticJsonDocument<1024> doc;
        GNSSManager::getSingleton()->statusJson(&doc);
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        serializeJson(doc, *response);
        request->send(response);
    });
    // PowerManager
    server->on("/powermanager/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        StaticJsonDocument<1024> doc;
        PowerManager::getSingleton()->statusJson(&doc);
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        serializeJson(doc, *response);
        request->send(response);
    });
    // StatsManager
        server->on("/statsmanager/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        StaticJsonDocument<1024> doc;
        StatsManager::getSingleton()->statusJson(&doc);
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        serializeJson(doc, *response);
        request->send(response);
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