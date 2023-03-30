#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <TBeamPower.h>

class PowerManager {
public:
    PowerManager();
    static PowerManager* getSingleton();
    void enablePeripherals();
    void statusJson(JsonDocument *doc);
private:
    TBeamPower *power;
};