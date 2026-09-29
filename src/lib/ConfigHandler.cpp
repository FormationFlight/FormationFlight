#include <Arduino.h>
#include "ConfigHandler.h"
#include <EEPROM.h>
#include "ConfigStrings.h"
#include "Follow/FollowManager.h"

void config_clear()
{
    for (int i = 0; i < 512; i++)
    {
        EEPROM.write(i, 0);
    }
    EEPROM.commit();
}

void config_save()
{
    for (size_t i = 0; i < sizeof(cfg); i++)
    {
        char data = ((char *)&cfg)[i];
        EEPROM.write(i, data);
    }
    EEPROM.commit();
}

void config_init(bool forcedefault)
{
    size_t cfgSize = sizeof(cfg);
    // Reserve cfg's own footprint plus FollowManager's persisted region,
    // which lives immediately after it (see
    // FollowManager::loadFromEEPROM()/saveToEEPROM()).
    EEPROM.begin(cfgSize + sizeof(FollowEepromRecord));

    for (size_t i = 0; i < cfgSize; i++)
    {
        char data = EEPROM.read(i);
        ((char *)&cfg)[i] = data;
    }

    if (cfg.version != VERSION_CONFIG || forcedefault)
    {
        cfg.version = VERSION_CONFIG;
        cfg.force_gs = false;

        cfg.lora_nodes = LORA_M3_NODES;
        cfg.slot_spacing = LORA_M3_SLOT_SPACING;
        cfg.lora_timing_delay = LORA_M3_TIMING_DELAY;
        cfg.msp_after_tx_delay = LORA_M3_MSP_AFTER_TX_DELAY;

        cfg.display_enable = 1;
        config_save();
    }
}