#include "StatsManager.h"

const char* keyFriendlyNames[StatsKeyCount] = {
    "radiomanager_looptime_us",
    "wifimanager_looptime_us",
    "peermanager_looptime_us",
    "gnssmanager_looptime_us",
    "mspmanager_looptime_us",
    "display_updatetime_us",
    "msp_sendtime_us",
    "ota_sendtime_us",
};

StatsManager::StatsManager()
{
}

StatsManager *statsManager = nullptr;

StatsManager *StatsManager::getSingleton()
{
    if (statsManager == nullptr)
    {
        statsManager = new StatsManager();
    }
    return statsManager;
}

void StatsManager::setValue(StatsKey key, unsigned long value)
{
    values[key][0] = value;
}
void StatsManager::storeTimerAndRestart(StatsKey key)
{
    setValue(key, micros() - runTimer);
    startTimer();
}
void StatsManager::startTimer()
{
    runTimer = micros();
}
void StatsManager::startEpoch()
{
    for (uint8_t i = 0; i < StatsKeyCount; i++)
    {
        for (uint8_t j = STATS_SAMPLES_COUNT - 1; j > 0; j--)
        {
            values[i][j] = values[i][j - 1];
        }
    }
}

void StatsManager::statusJson(JsonDocument *doc)
{
    for (uint8_t i = 0; i < StatsKeyCount; i++)
    {
        JsonArray o = doc->createNestedArray(keyFriendlyNames[i]);
        for (uint8_t j = 0; j < STATS_SAMPLES_COUNT; j++)
        {
            o.add(values[i][j]);
        }
    }
}

unsigned long StatsManager::getLatest(StatsKey key) {
    return values[key][0];
}
unsigned long StatsManager::getAverage(StatsKey key) {
    unsigned long sum = 0;
    for (uint8_t i = 0; i < STATS_SAMPLES_COUNT; i++)
    {
        sum += values[key][i];
    }
    return sum / STATS_SAMPLES_COUNT;
}
unsigned long StatsManager::getHighest(StatsKey key) {
    unsigned long highestSeen = 0;
    for (uint8_t i = 0; i < STATS_SAMPLES_COUNT; i++)
    {
        highestSeen = max(highestSeen, values[key][i]);
    }
    return highestSeen;
}