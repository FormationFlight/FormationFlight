#pragma once
#include <ESPAsyncWebServer.h>

#include "ConfigStore.h"
#include "MspFcLink.h"
#include "SimRadio.h"
#include "config.h"
#include "crypto.h"
#include "follow.h"
#include "node.h"
#include "radio_hub.h"

namespace ff {

// The node's WiFi bring-up and JSON web API.
//
// The contract this implements is written down in docs/v2-web-api.md, which the
// web UI and the development mock server are also built against. If you change
// a field name here, change it there, or you have broken the UI silently.
//
// Deliberately no websocket. The ESP8285 build already carries a LoRa driver, an
// MSP link and this server in about 40 KB of heap, and a websocket's per-client
// buffers were not worth what polling costs. The UI polls one endpoint per tick.
//
// Config changes apply to RAM immediately and are only persisted when the client
// asks. That split is what makes a setting that takes the node off the air
// recoverable with a power cycle instead of a reflash.
struct WebDeps {
    Settings* cfg = nullptr;
    ConfigStore* store = nullptr;
    Node* node = nullptr;
    RadioHub* hub = nullptr;
    FollowController* follow = nullptr;  // null on a listen-only build
    MspFcLink* fc = nullptr;
    SimRadio* sim = nullptr;             // null when the simulator is compiled out
    CcmCrypto* crypto = nullptr;         // null in plaintext passthrough mode
    ILocationSource* location = nullptr;
    const char* fw_version = "dev";
    uint32_t uid = 0;
    // Called after a successful config POST so the caller can push whatever can
    // be changed without a reboot into the live objects.
    void (*on_config_applied)(const Settings&) = nullptr;
};

// ESPAsyncWebServer calls upload handlers as plain functions, so these cannot be
// members; they reach the server through WebServer::instance().
void handleFileUploadData(AsyncWebServerRequest* request, const String& filename, size_t index,
                          uint8_t* data, size_t len, bool final);
void handleFileUploadResponse(AsyncWebServerRequest* request);

class WebServer {
public:
    // Brings up WiFi (AP or station per the config) and starts the server.
    void begin(const WebDeps& deps);
    void loop(uint32_t now_ms);

    bool otaActive() const { return ota_active_; }
    // True once a setting was changed that only takes effect at boot.
    bool rebootRequired() const { return reboot_required_; }
    void setRebootRequired() { reboot_required_ = true; }

    // Exposed for the upload handlers, which ESPAsyncWebServer calls as plain
    // functions rather than as members.
    static WebServer* instance() { return instance_; }
    WebDeps& deps() { return deps_; }
    void setOtaActive() { ota_active_ = true; }
    String& otaMessage() { return ota_message_; }
    uint16_t& otaStatus() { return ota_status_; }
    void requestReboot(uint32_t at_ms) { reboot_at_ms_ = at_ms; }

    // Minimum gap between writes of config.json.
    static constexpr uint32_t kSaveMinIntervalMs = 2000;
    bool saveAllowed(uint32_t now_ms) const {
        return !ever_saved_ || (now_ms - last_save_ms_) >= kSaveMinIntervalMs;
    }
    void markSaved(uint32_t now_ms) {
        ever_saved_ = true;
        last_save_ms_ = now_ms;
    }

private:
    void startWifi();
    void registerRoutes();

    static WebServer* instance_;
    WebDeps deps_;
    AsyncWebServer* server_ = nullptr;
    bool ota_active_ = false;
    bool reboot_required_ = false;
    uint32_t reboot_at_ms_ = 0;
    // Flash has a finite number of erase cycles and a UI with a Save button has
    // an infinite number of clicks.
    bool ever_saved_ = false;
    uint32_t last_save_ms_ = 0;
    String ota_message_;
    uint16_t ota_status_ = 0;
};

}  // namespace ff
