#include <Arduino.h>
#include "ConfigHandler.h"
#include <EEPROM.h>
#include "ConfigStrings.h"

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

    size_t size = sizeof(cfg);
    EEPROM.begin(size * 2);

    for (size_t i = 0; i < size; i++)
    {
        char data = EEPROM.read(i);
        ((char *)&cfg)[i] = data;
    }

    if (true || cfg.version != VERSION_CONFIG || forcedefault)
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