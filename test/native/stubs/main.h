#pragma once

// Native-only stand-in for the real src/main.h -- provides only what
// FollowManager.cpp actually reads from main.h: sys.phase (gates loop()) and
// cfg (its sizeof anchors FOLLOW_EEPROM_OFFSET). Deliberately does not pull
// in the real main.h, which drags in the full radio/OTA/WiFi stack.
// Resolved via this env's -I test/native/stubs, ordered ahead of -I src on
// the include path so FollowManager.cpp's #include "main.h" finds this one
// instead of the real src/main.h.

#include <cstdint>

#ifndef DEBUG
#define DEBUG 0
#endif
#define DBGLN(x) do { if (DEBUG) { Serial.printf("%lu: ", millis()); Serial.println(x); } } while (0)
#define DBGF(...) do { if (DEBUG) { Serial.printf("%lu: ", millis()); Serial.printf(__VA_ARGS__); } } while (0)

enum MODE {
    MODE_START = 0,
    MODE_HOST_SCAN = 1,
    MODE_OTA_SCAN = 2,
    MODE_OTA_SYNC = 3,
    MODE_OTA_RX = 4,
    MODE_OTA_TX = 5
};

struct config_t {
    uint16_t version;
};

struct system_t {
    // Defaults past MODE_OTA_SYNC so tests don't have to set this just to
    // get loop() past its sync gate -- override per-test to exercise it.
    MODE phase = MODE_OTA_TX;
};

extern config_t cfg;
extern system_t sys;
