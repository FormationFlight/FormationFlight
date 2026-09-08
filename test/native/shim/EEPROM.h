#pragma once

#include <cstdint>
#include <cstring>

// Minimal in-memory stand-in for the Arduino EEPROM library's get/put/commit
// API -- FollowManager.cpp's only use of EEPROM (see loadFromEEPROM()/
// saveToEEPROM()). Backing store is just a byte array; commit() always
// succeeds since there's no real flash to fail writing to.
class EEPROMClass {
public:
    void begin(size_t) {}
    template <class T> T &get(int address, T &t) {
        memcpy(&t, buf + address, sizeof(T));
        return t;
    }
    template <class T> const T &put(int address, const T &t) {
        memcpy(buf + address, &t, sizeof(T));
        return t;
    }
    bool commit() { return true; }
private:
    uint8_t buf[8192]{};
};

extern EEPROMClass EEPROM;
