#include "PowerManager.h"

PowerManager::PowerManager()
{
#ifdef TARGET_TBEAM
    power = new TBeamPower();
    power->begin();
#endif
}

PowerManager *powerManager = nullptr;

PowerManager* PowerManager::getSingleton()
{
    if (powerManager == nullptr)
    {
        powerManager = new PowerManager();
    }
    return powerManager;
}

void PowerManager::enablePeripherals()
{
#ifdef TARGET_TBEAM
    power->power_sensors(true);
    power->power_peripherals(true);
    power->power_GPS(true);
    power->power_LoRa(true);
#endif
}

void PowerManager::statusJson(JsonDocument *doc)
{
#ifdef TARGET_TBEAM
    JsonObject o = doc->createNestedObject("voltages");
    o["batteryVoltage"] = power->get_battery_voltage();
    o["supplyVoltage"] = power->get_supply_voltage();
#endif
}