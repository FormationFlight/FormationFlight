#include "GNSSManager.h"

GNSSManager::GNSSManager()
{

}

GNSSManager *gnssManager = nullptr;

GNSSManager* GNSSManager::getSingleton()
{
    if (gnssManager == nullptr)
    {
        gnssManager = new GNSSManager();
    }
    return gnssManager;
}

GNSSLocation GNSSManager::getLocation()
{
    // Fetch the first provider that has a good fix
    for (uint8_t i = 0; i < GNSS_MAX_PROVIDERS; i++) {
        if (providers[i] != nullptr && providers[i]->getLocation().fixType != 0) {
            return providers[i]->getLocation();
        }
    }
}

void GNSSManager::addProvider(GNSSProvider *provider)
{
    for (uint8_t i = 0; i < GNSS_MAX_PROVIDERS; i++) {
        if (providers[i] == nullptr) {
            providers[i] = provider;
            providers[i]->begin();
            return;
        }
    }
}

void GNSSManager::loop()
{
    for (uint8_t i = 0; i < GNSS_MAX_PROVIDERS; i++) {
        if (providers[i] == nullptr) {
            continue;
        }
        providers[i]->loop();
    }
}