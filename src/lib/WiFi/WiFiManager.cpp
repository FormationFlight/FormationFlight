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
#include "webcontent.h"

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
    // Permit cross-origin requests
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, PUT");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");
    server->on("/system/status", HTTP_GET, handleSystemStatus);
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
    server->on("/system/status", HTTP_GET, handleSystemStatus);
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
        StaticJsonDocument<2048> doc;
        PeerManager::getSingleton()->statusJson(&doc);
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        serializeJson(doc, *response);
        request->send(response);
    });
    server->on("/peermanager/spoof", HTTP_POST, [](AsyncWebServerRequest *request) {
        PeerManager::getSingleton()->enableSpoofing(true);
        request->send(200, "text/plain", "OK");
    });
    // MSPManager
    server->on("/mspmanager/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        StaticJsonDocument<1024> doc;
        MSPManager::getSingleton()->statusJson(&doc);
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
    server->on("/gnssmanager/spoof", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!request->hasParam("lat", true) || !request->hasParam("lon", true)) {
            request->send(400, "text/plain", "need parameters lat & lon");
            return;
        }
        double lat = request->getParam("lat", true)->value().toDouble();
        double lon = request->getParam("lon", true)->value().toDouble();
        GNSSManager::getSingleton()->spoofedLocation.lat = lat;
        GNSSManager::getSingleton()->spoofedLocation.lon = lon;
        GNSSManager::getSingleton()->spoofedLocation.fixType = GNSS_FIX_TYPE_3D;
        GNSSManager::getSingleton()->spoofedLocation.alt = 42000; // cm
        GNSSManager::getSingleton()->spoofedLocation.numSat = 42;
        GNSSManager::getSingleton()->spoofedLocation.hdop = 0.69;
        GNSSManager::getSingleton()->spoofLocationEnabled = true;

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
    // 404
    server->onNotFound([](AsyncWebServerRequest *request) {
        // Handle CORS Preflight
        if (request->method() == HTTP_OPTIONS) {
            request->send(200);
        } else {
            request->send(404, "text/plain", "Not found");
        }
    });
    #include "staticfilehandler.inc"
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
    return otaCompleteAt > 0 && millis() - otaCompleteAt > 1500;
}

void WiFiManager::setOTAComplete()
{
    otaCompleteAt = millis();
}

OTAResult* WiFiManager::getOTAResult()
{
    return &otaResult;
}

void handleSystemStatus(AsyncWebServerRequest *request)
{
    StaticJsonDocument<512> doc;
    doc["target"] = CFG_TARGET_FULLNAME;
#ifdef PLATFORM_ESP8266
    doc["platform"] = "ESP8266";
#elif defined(PLATFORM_ESP32)
    doc["platform"] = "ESP32";
#endif
    doc["version"] = VERSION;
    doc["gitHash"] = GITHASH;
    doc["buildTime"] = BUILDTIME;
    doc["cloudBuild"] = CLOUD_BUILD;
    doc["heap"] = ESP.getFreeHeap();
#ifdef LORA_BAND
    doc["lora_band"] = LORA_BAND;
#endif
    doc["uptimeMilliseconds"] = millis();
    doc["phase"] = sys.phase;
    doc["name"] = curr.name;
    doc["longName"] = generate_id();
    doc["host"] = host_name[MSPManager::getSingleton()->getFCVariant()];
    doc["state"] = MSPManager::getSingleton()->getState();
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response);
}

void handleFileUploadResponse(AsyncWebServerRequest *request)
{
    OTAResult *r = WiFiManager::getSingleton()->getOTAResult();
    if (r->statusCode == 0)
    {
        return;
    }
    else if (r->statusCode == 200)
    {
        AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", "Please wait while the device reboots...");
        response->addHeader("Refresh", "15");
        response->addHeader("Location", "/");
        response->addHeader("Connection", "close");
        request->send(response);
        request->client()->close();
    }
    else
    {
        request->send(r->statusCode, "text/plain", r->message);
    }

}

void handleFileUploadData(AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len, bool final)
{
    OTAResult *r = WiFiManager::getSingleton()->getOTAResult();
#ifdef PLATFORM_ESP8266
    if (!filename.endsWith(".bin") && !filename.endsWith(".bin.gz")) {
        r->message = "must upload .bin or .bin.gz";
#elif defined(PLATFORM_ESP32)
    if (!filename.endsWith(".bin")) {
        r->message = "must upload .bin";
#endif
        r->statusCode = 400;
        return;
    }
    if (!index && !Update.isRunning())
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
            r->message = Update.getErrorString();
#elif defined(PLATFORM_ESP32)
            r->message = Update.errorString();
#endif
            r->statusCode = 500;
            return;
        }
        WiFiManager::getSingleton()->setOTAActive();
    }

    if (Update.write(data, len) != len)
    {
        Update.printError(Serial);
#ifdef PLATFORM_ESP8266
        r->message = Update.getErrorString();
#elif defined(PLATFORM_ESP32)
        r->message = Update.errorString();
#endif
        r->statusCode = 500;
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
            r->statusCode = 200;
            return;
        }
    }
}
