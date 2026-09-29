#pragma once
#include "config.h"

namespace ff {

// Persists the configuration as JSON in LittleFS.
//
// Replaces v1's packed struct in EEPROM and v2's earlier Follow-only EEPROM
// record. A file has three properties the struct did not: it survives adding a
// field, it can be read and repaired with esptool and a text editor, and it can
// be backed up and restored between nodes.
//
// Two failure modes are handled deliberately, both learned from field units:
//
//   - Mount failure is retried before the filesystem is formatted. Formatting on
//     the first hiccup destroys the WiFi credentials of a node that is somewhere
//     inconvenient, which is exactly when you need them.
//   - A file that exists but will not parse is LEFT ALONE. Missing means first
//     boot, and defaults are written. Unparseable means something is wrong, and
//     overwriting it destroys the evidence and any chance of recovery; this boot
//     runs on RAM defaults instead, and a power cycle gets another go.
//
class ConfigStore {
public:
    // Mounts the filesystem. Returns false if it could not be mounted at all, in
    // which case load() and save() will fail and the node runs on defaults.
    bool begin();

    // Reads config.json into `cfg`. Returns true if a stored config was applied.
    // False means defaults are in force: either first boot (in which case the
    // defaults have been written out) or an unreadable file (which has not been
    // touched).
    bool load(Settings& cfg);

    // Serializes `cfg` to config.json. Writes to a temporary file and renames,
    // so an interrupted write (a brown-out mid-save) cannot leave a truncated
    // config behind: either the old file or the new one survives intact.
    bool save(const Settings& cfg, const char** err = nullptr);

    // Deletes the stored config. The next boot starts from defaults.
    bool erase();

    bool mounted() const { return mounted_; }
    // True when the last load() found a file it could not parse, so the UI can
    // warn that the running config is not the saved one.
    bool lastLoadCorrupt() const { return corrupt_; }

private:
    bool mounted_ = false;
    bool corrupt_ = false;
};

}  // namespace ff
