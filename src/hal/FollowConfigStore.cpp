#include "FollowConfigStore.h"

#include <Arduino.h>
#include <EEPROM.h>

namespace ff {

void FollowConfigStore::begin() {
    EEPROM.begin(kFollowEepromOffset + sizeof(FollowRecord));
}

bool FollowConfigStore::load(FollowController& ctl) {
    FollowRecord rec;
    EEPROM.get(kFollowEepromOffset, rec);
    return ctl.loadRecord(rec);
}

bool FollowConfigStore::save(FollowController& ctl, uint32_t now_ms, const char** err) {
    FollowRecord rec;
    if (!ctl.requestSave(now_ms, rec, err)) {
        return false;
    }
    EEPROM.put(kFollowEepromOffset, rec);
    if (!EEPROM.commit()) {
        if (err) *err = "EEPROM commit failed";
        return false;
    }
    return true;
}

}  // namespace ff
