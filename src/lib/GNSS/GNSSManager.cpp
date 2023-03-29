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
    static uint32_t lastUpdate = 0;
    static GNSSLocation loc;
    if (millis() - lastUpdate > GNSS_FRESH_INTERVAL_MS) {
        // Fetch the first provider that has a good fix
        for (uint8_t i = 0; i < GNSS_MAX_PROVIDERS; i++) {
            if (providers[i] != nullptr) {
                GNSSLocation providerLoc = providers[i]->getLocation();
                if (providerLoc.fixType != 0) {
                    loc = providerLoc;
                    break;
                }
            }
        }
    }
    return loc;
}

void GNSSManager::addProvider(GNSSProvider *provider)
{
    for (uint8_t i = 0; i < GNSS_MAX_PROVIDERS; i++) {
        if (providers[i] == nullptr) {
            providers[i] = provider;
            return;
        }
    }
}

void GNSSManager::addListener(GNSSListener *listener)
{
       for (uint8_t i = 0; i < GNSS_MAX_LISTENERS; i++) {
        if (listeners[i] == nullptr) {
            listeners[i] = listener;
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

void GNSSManager::statusJson(JsonDocument *doc)
{
    JsonArray providersArray = doc->createNestedArray("providers");
    for (uint8_t i = 0; i < GNSS_MAX_PROVIDERS; i++)
    {
        if (providers[i] != nullptr) {
            GNSSProvider *provider = providers[i];
            JsonObject o = providersArray.createNestedObject();
            o["status"] = provider->getStatusString();
            o["enabled"] = provider->getEnabled();
        }
    }
}


void GNSSManager::setProviderStatus(uint8_t index, bool status)
{
    if (providers[index] == nullptr) {
        return;
    }
    providers[index]->setEnabled(status);
}