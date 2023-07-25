#pragma once

#include <ESPAsyncWebServer.h>

struct OTAResult {
    uint16_t statusCode = 0;
    String message = "";
};

class WiFiManager
{
public:
    WiFiManager();
    static WiFiManager *getSingleton();
    void loop();
    void setOTAActive();
    bool getOTAActive();
    void setOTAComplete();
    bool getOTAComplete();
    OTAResult *getOTAResult();
private:
    AsyncWebServer *server;
    bool otaActive = false;
    unsigned long otaCompleteAt = 0;
    OTAResult otaResult;
};

void OnOTAStart();
void handleSystemStatus(AsyncWebServerRequest *request);
void handleFileUploadData(AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len, bool final);
void handleFileUploadResponse(AsyncWebServerRequest *request);
void handleSystemStatus(AsyncWebServerRequest *request);