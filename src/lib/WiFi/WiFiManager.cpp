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
#include "../Follow/FollowManager.h"
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
        // With sideLength, send a spoofed peer around a closed hexagon patrol path (repeatable
        // bench testing of FollowManager against a moving target), centered on lat/lon if given,
        // or on our own current GNSS fix at POST time if not - so it can be triggered without
        // knowing coordinates up front. With just lat & lon (no sideLength), position a single
        // spoofed peer explicitly instead. Without any of that, fall back to the original fixed
        // 100m-ring of 5 peers.
        if (request->hasParam("sideLength", true)) {
            uint8_t index = request->hasParam("index", true) ? request->getParam("index", true)->value().toInt() : 0;
            double sideLength = request->getParam("sideLength", true)->value().toDouble();
            double speed = request->hasParam("speed", true) ? request->getParam("speed", true)->value().toDouble() : 0;
            double lat, lon;
            if (request->hasParam("lat", true) && request->hasParam("lon", true)) {
                lat = request->getParam("lat", true)->value().toDouble();
                lon = request->getParam("lon", true)->value().toDouble();
            } else {
                GNSSLocation loc = GNSSManager::getSingleton()->getLocation();
                lat = loc.lat;
                lon = loc.lon;
            }
            PeerManager::getSingleton()->spoofPeerHexPath(index, lat, lon, sideLength, speed);
        } else if (request->hasParam("lat", true) && request->hasParam("lon", true)) {
            uint8_t index = request->hasParam("index", true) ? request->getParam("index", true)->value().toInt() : 0;
            double lat = request->getParam("lat", true)->value().toDouble();
            double lon = request->getParam("lon", true)->value().toDouble();
            double speed = request->hasParam("speed", true) ? request->getParam("speed", true)->value().toDouble() : 0;
            double course = request->hasParam("course", true) ? request->getParam("course", true)->value().toDouble() : 0;
            PeerManager::getSingleton()->spoofPeer(index, lat, lon, course, speed);
        } else {
            PeerManager::getSingleton()->enableSpoofing(true);
        }
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
    // FollowManager
    server->on("/followmanager/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        StaticJsonDocument<896> doc;
        FollowManager::getSingleton()->statusJson(&doc);
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        serializeJson(doc, *response);
        request->send(response);
    });
    server->on("/followmanager/config", HTTP_GET, [](AsyncWebServerRequest *request) {
        StaticJsonDocument<1280> doc;
        FollowManager::getSingleton()->configJson(&doc);
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        serializeJson(doc, *response);
        request->send(response);
    });
    // Live config update: applies to the in-memory config only, not EEPROM
    // (see the /followmanager/commit handler below for that). Accepts a
    // subset of form params, applies them on top of the current config,
    // validates the result (offset geometry + basic field sanity) and
    // rejects the whole update if it fails, rather than partially applying it.
    server->on("/followmanager/config", HTTP_POST, handleFollowManagerConfigPost);
    // Explicit "flush the current in-memory config to EEPROM" action,
    // distinct from the live-edit POST above. Rate-limited inside
    // FollowManager::saveToEEPROM() so rapid/duplicate clicks don't hammer
    // EEPROM with writes.
    server->on("/followmanager/commit", HTTP_POST, [](AsyncWebServerRequest *request) {
        String errMsg;
        if (!FollowManager::getSingleton()->saveToEEPROM(&errMsg)) {
            request->send(429, "text/plain", errMsg);
            return;
        }
        request->send(200, "text/plain", "OK");
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

// Parses a subset of FollowRuntimeConfig's fields from POST form params,
// applies them on top of the current in-memory FollowManager config, and asks
// FollowManager to validate+swap the result atomically. Any single
// unrecognized enum value fails the whole request with 400 before anything
// is applied, so a typo'd param can't silently leave other fields updated.
void handleFollowManagerConfigPost(AsyncWebServerRequest *request)
{
    FollowRuntimeConfig cfg = FollowManager::getSingleton()->getConfig();

    auto strParam = [&](const char *name) {
        return request->getParam(name, true)->value();
    };

    // Canonical track-relative offset — the AHEAD/BEHIND/etc.
    // "friendly grid" is a client-side view over these signed meters
    // (html/follow.js); the server only ever sees this one representation.
    if (request->hasParam("ofsLongM", true)) cfg.ofsLongM = strParam("ofsLongM").toDouble();
    if (request->hasParam("ofsLatM", true)) cfg.ofsLatM = strParam("ofsLatM").toDouble();
    if (request->hasParam("ofsVertM", true)) cfg.ofsVertM = strParam("ofsVertM").toDouble();

    if (request->hasParam("targetPeer", true)) cfg.targetPeer = (uint8_t)strParam("targetPeer").toInt();
    if (request->hasParam("emitHz", true)) cfg.emitHz = (uint16_t)strParam("emitHz").toInt();
    if (request->hasParam("peerTimeoutMs", true)) cfg.peerTimeoutMs = (uint32_t)strParam("peerTimeoutMs").toInt();

    if (request->hasParam("minSepM", true)) cfg.minSepM = strParam("minSepM").toDouble();
    if (request->hasParam("minVSepM", true)) cfg.minVSepM = strParam("minVSepM").toDouble();
    if (request->hasParam("maxTargetDistM", true)) cfg.maxTargetDistM = strParam("maxTargetDistM").toDouble();
    if (request->hasParam("minAltM", true)) cfg.minAltM = strParam("minAltM").toDouble();
    if (request->hasParam("minCourseSpeed", true)) cfg.minCourseSpeed = strParam("minCourseSpeed").toDouble();

    // Nose heading — headingDeg is shared between FIXED
    // (absolute) and COURSE_RELATIVE (offset from course); which
    // interpretation applies depends solely on headingMode.
    if (request->hasParam("headingMode", true)) {
        String v = strParam("headingMode");
        if (v == "OFF") cfg.headingMode = FOLLOW_HEADING_OFF;
        else if (v == "COURSE") cfg.headingMode = FOLLOW_HEADING_COURSE;
        else if (v == "POINT_LEADER") cfg.headingMode = FOLLOW_HEADING_POINT_LEADER;
        else if (v == "FIXED") cfg.headingMode = FOLLOW_HEADING_FIXED;
        else if (v == "COURSE_RELATIVE") cfg.headingMode = FOLLOW_HEADING_COURSE_RELATIVE;
        else { request->send(400, "text/plain", "invalid headingMode (want OFF/COURSE/POINT_LEADER/FIXED/COURSE_RELATIVE)"); return; }
    }
    if (request->hasParam("headingDeg", true)) cfg.headingDeg = strParam("headingDeg").toDouble();

    if (request->hasParam("statusGvarIndex", true)) cfg.statusGvarIndex = (int16_t)strParam("statusGvarIndex").toInt();
    if (request->hasParam("conditionFlagsGvarIndex", true)) cfg.conditionFlagsGvarIndex = (int16_t)strParam("conditionFlagsGvarIndex").toInt();

    if (request->hasParam("rcLongChannel", true)) cfg.rcLongChannel = (int16_t)strParam("rcLongChannel").toInt();
    if (request->hasParam("rcLatChannel", true)) cfg.rcLatChannel = (int16_t)strParam("rcLatChannel").toInt();
    if (request->hasParam("rcVertChannel", true)) cfg.rcVertChannel = (int16_t)strParam("rcVertChannel").toInt();

    // Speed autothrottle.
    if (request->hasParam("targetSpeedGvarIndex", true)) cfg.targetSpeedGvarIndex = (int16_t)strParam("targetSpeedGvarIndex").toInt();
    if (request->hasParam("autothrottleEngageGvarIndex", true)) cfg.autothrottleEngageGvarIndex = (int16_t)strParam("autothrottleEngageGvarIndex").toInt();
    if (request->hasParam("autothrottleEnableRcChannel", true)) cfg.autothrottleEnableRcChannel = (int16_t)strParam("autothrottleEnableRcChannel").toInt();
    if (request->hasParam("autothrottleEnableMinThresholdUs", true)) cfg.autothrottleEnableMinThresholdUs = (int16_t)strParam("autothrottleEnableMinThresholdUs").toInt();
    if (request->hasParam("autothrottleEnableMaxThresholdUs", true)) cfg.autothrottleEnableMaxThresholdUs = (int16_t)strParam("autothrottleEnableMaxThresholdUs").toInt();
    if (request->hasParam("speedCorrectionAccelCmS2", true)) cfg.speedCorrectionAccelCmS2 = (int16_t)strParam("speedCorrectionAccelCmS2").toInt();
    if (request->hasParam("minTargetSpeedMps", true)) cfg.minTargetSpeedMps = strParam("minTargetSpeedMps").toDouble();
    if (request->hasParam("maxTargetSpeedMps", true)) cfg.maxTargetSpeedMps = strParam("maxTargetSpeedMps").toDouble();

    // RAM only (never persisted — see FollowRuntimeConfig::debug).
    if (request->hasParam("debug", true)) cfg.debug = strParam("debug") == "true";

    String errMsg;
    if (!FollowManager::getSingleton()->applyConfig(cfg, &errMsg)) {
        request->send(400, "text/plain", errMsg);
        return;
    }

    StaticJsonDocument<896> doc;
    FollowManager::getSingleton()->configJson(&doc);
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
