#include "ConfigManager.h"

ConfigManager::ConfigManager()
{

}

void reset_configuration(Configuration *config)
{
}

ConfigManager *configManager = nullptr;

ConfigManager *ConfigManager::getSingleton()
{
    if (configManager == nullptr)
    {
        configManager = new ConfigManager();
    }
    return configManager;
}

void ConfigManager::save()
{
    for (size_t i = 0; i < sizeof(config); i++)
    {
        char data = ((char *)&config)[i];
        EEPROM.write(i, data);
    }
    EEPROM.commit();
}