#pragma once
#include "follow.h"

namespace ff {

// EEPROM-backed persistence for the Follow configuration.
//
// Holds one versioned FollowRecord at kFollowEepromOffset. The controller does
// the validation and the save rate-limiting (FollowController::loadRecord /
// requestSave); this class is only the flash plumbing, so it is the one piece
// of Follow that is not host-tested.
//
// v2 has no other persisted configuration yet (that is Phase 3's ConfigManager).
// When it arrives it should either own this record inside its own versioned
// blob or place its data after sizeof(FollowRecord); until then this region
// starts at offset 0.
constexpr int kFollowEepromOffset = 0;

class FollowConfigStore {
public:
    // Brings up the EEPROM emulation sized for the record. Call once at boot.
    void begin();

    // Reads the stored record and applies it if valid. Returns whether a
    // stored config was applied (false = defaults stay in force).
    bool load(FollowController& ctl);

    // Commits the controller's current config. Returns false (with *err set) if
    // the controller's rate limit refused, or the commit failed.
    bool save(FollowController& ctl, uint32_t now_ms, const char** err = nullptr);
};

}  // namespace ff
