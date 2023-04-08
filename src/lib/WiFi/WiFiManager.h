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