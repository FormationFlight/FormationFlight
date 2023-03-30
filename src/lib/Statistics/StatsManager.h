#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#define STATS_SAMPLES_COUNT 8

enum StatsKey {
    STATS_KEY_RADIOMANAGER_LOOPTIME_MS = 0,
    STATS_KEY_WIFIMANAGER_LOOPTIME_MS,
    STATS_KEY_PEERMANAGER_LOOPTIME_MS,
    STATS_KEY_GNSSMANAGER_LOOPTIME_MS,
    STATS_KEY_DISPLAY_UPDATETIME_MS,
    STATS_KEY_MSP_SENDTIME_MS,
    STATS_KEY_OTA_SENDTIME_MS,
    StatsKeyCount,
};

extern const char* keyFriendlyNames[StatsKeyCount];

class StatsManager {
public:
    StatsManager();
    static StatsManager* getSingleton();
    // Starts a new set of timers for a new loop iteration
    void startEpoch();
    void setValue(StatsKey key, unsigned long value);
    void storeTimerAndRestart(StatsKey key);
    void startTimer();
    unsigned long getLatest(StatsKey key);
    unsigned long getAverage(StatsKey key);
    unsigned long getHighest(StatsKey key);
    void statusJson(JsonDocument *doc);
private:
    unsigned long runTimer;
    unsigned long values[StatsKeyCount][STATS_SAMPLES_COUNT] = {0};
};