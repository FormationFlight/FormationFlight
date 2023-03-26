#pragma once

#include <ESPAsyncWebServer.h>

class WiFiManager {
    public:
    WiFiManager();
    static WiFiManager* getSingleton();
    void loop();
    private:
    AsyncWebServer *server;
};