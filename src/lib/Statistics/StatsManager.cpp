#include "StatsManager.h"

const char* keyFriendlyNames[StatsKeyCount] = {
    "radiomanager_looptime_ms",
    "wifimanager_looptime_ms",
    "peermanager_looptime_ms",
    "gnssmanager_looptime_ms",
    "display_updatetime_ms",
    "msp_sendtime_ms",
    "ota_sendtime_ms",
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
    setValue(key, millis() - runTimer);
    startTimer();
}
void StatsManager::startTimer()
{
    runTimer = millis();
}
void StatsManager::startEpoch()
{
    for (uint8_t i = 0; i < StatsKeyCount; i++)
    {
        for (uint8_t j = STATS_SAMPLES_COUNT - 1; j > 0; j--)
        {
            values[i][j + 1] = values[i][j];
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