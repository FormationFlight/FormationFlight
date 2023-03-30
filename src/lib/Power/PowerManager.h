#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#ifdef TARGET_TBEAM
#include <TBeamPower.h>
#endif

class PowerManager {
public:
    PowerManager();
    static PowerManager* getSingleton();
    void enablePeripherals();
    void statusJson(JsonDocument *doc);
private:
#ifdef TARGET_TBEAM
    TBeamPower *power;
#endif
};