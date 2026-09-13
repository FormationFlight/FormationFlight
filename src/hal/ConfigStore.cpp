#include "ConfigStore.h"

#include <Arduino.h>
#include <LittleFS.h>

namespace ff {

namespace {
constexpr const char* kConfigPath = "/config.json";
constexpr const char* kTempPath = "/config.new";
// Comfortably larger than the document, which is around 1.5 KB with the whole
// Follow block. Static so the parse does not need a heap allocation of this size
// on an ESP8285 whose heap is already carrying a web server.
constexpr size_t kJsonCapacity = 4096;
}  // namespace

bool ConfigStore::begin() {
#if defined(PLATFORM_ESP8266)
    mounted_ = LittleFS.begin();
#else
    mounted_ = LittleFS.begin(false);
#endif
    if (!mounted_) {
        delay(100);
#if defined(PLATFORM_ESP8266)
        mounted_ = LittleFS.begin();
#else
        mounted_ = LittleFS.begin(false);
#endif
    }
    if (!mounted_) {
        // Only now, having failed twice, format. This wipes the stored config,
        // so it is the last resort rather than the first.
#if defined(PLATFORM_ESP8266)
        LittleFS.format();
        mounted_ = LittleFS.begin();
#else
        mounted_ = LittleFS.begin(true);
#endif
    }
    return mounted_;
}

bool ConfigStore::load(Settings& cfg) {
    corrupt_ = false;
    if (!mounted_) {
        return false;
    }

    File f = LittleFS.open(kConfigPath, "r");
    if (!f || f.isDirectory() || f.size() == 0) {
        if (f) {
            f.close();
        }
        // Genuinely nothing saved: first boot. Writing the defaults out now
        // means the file exists and can be edited from the next boot onward.
        save(cfg, nullptr);
        return false;
    }

    DynamicJsonDocument doc(kJsonCapacity);
    const DeserializationError e = deserializeJson(doc, f);
    f.close();
    if (e) {
        // The file exists but will not parse. Leave it exactly as it is.
        corrupt_ = true;
        return false;
    }

    // Merge rather than replace, so a file written by older firmware simply
    // leaves newer fields at their defaults.
    const char* err = nullptr;
    if (!configMergeJson(doc.as<JsonObjectConst>(), cfg, &err)) {
        // Parsed, but the contents are not a valid configuration: a hand-edit
        // gone wrong, or a field whose rules tightened in a firmware update.
        // Same reasoning as above -- do not overwrite it.
        corrupt_ = true;
        return false;
    }
    return true;
}

bool ConfigStore::save(const Settings& cfg, const char** err) {
    const char* local = nullptr;
    if (err == nullptr) {
        err = &local;
    }
    if (!mounted_) {
        *err = "filesystem not mounted";
        return false;
    }

    DynamicJsonDocument doc(kJsonCapacity);
    JsonObject root = doc.to<JsonObject>();
    configToJson(cfg, root, /*redact_secrets=*/false);
    if (doc.overflowed()) {
        *err = "config too large to serialize";
        return false;
    }

    File f = LittleFS.open(kTempPath, "w");
    if (!f) {
        *err = "could not open config for writing";
        return false;
    }
    const size_t written = serializeJson(doc, f);
    f.close();
    if (written == 0) {
        LittleFS.remove(kTempPath);
        *err = "config write failed";
        return false;
    }

    // Rename over the old file. A brown-out before this point leaves the
    // previous config intact; after it, the new one. There is no window in
    // which a half-written file is the live config.
    LittleFS.remove(kConfigPath);
    if (!LittleFS.rename(kTempPath, kConfigPath)) {
        *err = "could not commit config";
        return false;
    }
    return true;
}

bool ConfigStore::erase() {
    if (!mounted_) {
        return false;
    }
    return LittleFS.remove(kConfigPath);
}

}  // namespace ff
