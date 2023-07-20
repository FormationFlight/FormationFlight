#pragma once

#include <ESPAsyncWebServer.h>

class WiFiManager
{
public:
    WiFiManager();
    static WiFiManager *getSingleton();
    void loop();
    void setOTAActive();
    bool getOTAActive();

private:
    AsyncWebServer *server;
    bool otaActive = false;
};

void OnOTAStart();
void handleFileUploadData(AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len, bool final);
void handleFileUploadResponse(AsyncWebServerRequest *request);