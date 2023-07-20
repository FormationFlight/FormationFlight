#include "WiFiManager.h"
#include "../Helpers.h"
#ifdef PLATFORM_ESP8266
#include <ESP8266WiFi.h>
#elif defined(PLATFORM_ESP32)
#include <WiFi.h>
#endif
// Friendly strings
#include "../ConfigStrings.h"
// OTA
#include <ArduinoOTA.h>
// Config methods
#include <ArduinoJson.h>
#include "../Radios/RadioManager.h"
#include "../Peers/PeerManager.h"
#include "../GNSS/GNSSManager.h"
#include "../Power/PowerManager.h"
#include "../Statistics/StatsManager.h"
#include "../Cryptography/CryptoManager.h"

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
        StaticJsonDocument<512> doc;
        doc["target"] = CFG_TARGET_FULLNAME;
        doc["version"] = VERSION;
        doc["buildTime"] = BUILDTIME;
        doc["cloudBuild"] = CLOUD_BUILD;
        doc["heap"] = ESP.getFreeHeap();
#ifdef LORA_BAND
        doc["lora_band"] = LORA_BAND;
#endif
        doc["uptimeMilliseconds"] = millis();
        doc["name"] = curr.name;
        doc["longName"] = generate_id();
        doc["host"] = host_name[MSPManager::getSingleton()->getFCVariant()];
        doc["state"] = MSPManager::getSingleton()->getState();
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        serializeJson(doc, *response);
        request->send(response);
    });
    server->on("/system/shutdown", HTTP_POST, [](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", "OK");
        ESP.deepSleep(UINT32_MAX);
    });
    server->on("/system/reboot", HTTP_POST, [](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", "OK");
#ifdef PLATFORM_ESP8266
        ESP.reset();
#elif defined(PLATFORM_ESP32)
        ESP.restart();
#endif
    });
    server->on("/system/bootloader", HTTP_POST, [](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", "OK");
#ifdef PLATFORM_ESP8266
        ESP.rebootIntoUartDownloadMode();
#elif defined(PLATFORM_ESP32)
        ESP.restart();
#endif
    });
    server->on("/system/delay", HTTP_POST, [](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", "OK");
        delayMicroseconds(1000);
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
    // MSPManager
    server->on("/mspmanager/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        StaticJsonDocument<1024> doc;
        MSPManager::getSingleton()->statusJson(&doc);
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        serializeJson(doc, *response);
        request->send(response);
    });
    server->on("/mspmanager/spoof", HTTP_POST, [](AsyncWebServerRequest *request) {
        MSPManager::getSingleton()->enableSpoofing(true);
        request->send(200, "text/plain", "OK");
    });
    // GNSSManager
    server->on("/gnssmanager/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        StaticJsonDocument<1024> doc;
        GNSSManager::getSingleton()->statusJson(&doc);
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        serializeJson(doc, *response);
        request->send(response);
    });
    server->on("/gnssmanager/spoof", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!request->hasParam("lat", true) || !request->hasParam("lon", true)) {
            request->send(400, "text/plain", "need parameters lat & lon");
            return;
        }
        double lat = request->getParam("lat", true)->value().toDouble();
        double lon = request->getParam("lon", true)->value().toDouble();
        GNSSManager::getSingleton()->spoofedLocation.lat = lat;
        GNSSManager::getSingleton()->spoofedLocation.lon = lon;

        request->send(200, "text/plain", "OK");
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
    // CryptoManager
    server->on("/cryptomanager/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        StaticJsonDocument<1024> doc;
        CryptoManager::getSingleton()->statusJson(&doc);
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        serializeJson(doc, *response);
        request->send(response);
    });
    // OTA firmware updates
    server->on("/update", HTTP_POST, handleFileUploadResponse, handleFileUploadData);
    server->begin();
    // Setup OTA updates
    ArduinoOTA.begin();
    ArduinoOTA.onStart(OnOTAStart);
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
    if (this->getOTAComplete()) {
#ifdef PLATFORM_ESP8266
        ESP.reset();
#elif defined(PLATFORM_ESP32)
        ESP.restart();
#endif
    }
}

void OnOTAStart()
{
    WiFiManager::getSingleton()->setOTAActive();
}

bool WiFiManager::getOTAActive()
{
    return otaActive;
}

void WiFiManager::setOTAActive()
{
    otaActive = true;
}

bool WiFiManager::getOTAComplete()
{
    return otaCompleteAt > 0 && millis() - otaCompleteAt > 500;
}

void WiFiManager::setOTAComplete()
{
    otaCompleteAt = millis();
}


void handleFileUploadResponse(AsyncWebServerRequest *request)
{
        AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", "Please wait while the device reboots...");
        // We can send them back to the homepage once we have a homepage :)
        /*response->addHeader("Refresh", "15");
        response->addHeader("Location", "/");
        response->addHeader("Connection", "close");*/
        request->send(response);
        request->client()->close();

}

void handleFileUploadData(AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len, bool final)
{
    if (!filename.endsWith(".bin") && !filename.endsWith(".elf")) {
        request->send(400, "text/plain", "must upload .bin or .elf");
    }
    if (!index)
    {
        size_t updateLength = request->contentLength();
        DBGF("HTTP update started with filename %s and size %d bytes\n", filename.c_str(), updateLength);
#ifdef PLATFORM_ESP8266
        Update.runAsync(true);
#endif
        if (!Update.begin(updateLength, U_FLASH))
        {
            Update.printError(Serial);
#ifdef PLATFORM_ESP8266
            request->send(500, "text/plain", Update.getErrorString());
#elif defined(PLATFORM_ESP32)
            request->send(500, "text/plain", Update.errorString());
#endif
            return;
        }
    }

    if (Update.write(data, len) != len)
    {
        Update.printError(Serial);
#ifdef PLATFORM_ESP8266
        request->send(500, "text/plain", Update.getErrorString());
#elif defined(PLATFORM_ESP32)
        request->send(500, "text/plain", Update.errorString());
#endif
        return;
    }
    else
    {
        static uint8_t previousPercentComplete = 255;
        uint8_t percentComplete = Update.progress() * 100 / Update.size();
        if (percentComplete != previousPercentComplete) {
            DBGF("Progress: %d%%\n", percentComplete);
            previousPercentComplete = percentComplete;
        }
    }

    if (final)
    {
        if (!Update.end(true))
        {
            Update.printError(Serial);
        }
        else
        {
            DBGLN("Update complete");
            WiFiManager::getSingleton()->setOTAComplete();
        }
    }
}