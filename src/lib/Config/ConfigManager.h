#pragma once

#include <EEPROM.h>
#include "../Radios/RadioManager.h"

struct RadioConfiguration {
    bool enabled;
    uint8_t powerLevel;
};

struct CryptoConfiguration {
    bool enabled;
    uint8_t groupKey[16];
};

struct Configuration {
    uint16_t configVersion;
    CryptoConfiguration cryptoConfiguration;
    RadioConfiguration radioConfiguration[MAX_RADIOS];
};

class ConfigManager
{
public:
    ConfigManager();
    static ConfigManager *getSingleton();

    void statusJson(JsonDocument *doc);
    void save();
private:
    Configuration config;
};