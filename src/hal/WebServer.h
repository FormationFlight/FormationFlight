#pragma once
#include <ESPAsyncWebServer.h>
#if defined(PLATFORM_ESP8266)
#include <Updater.h>
#else
#include <Update.h>
#endif

#include "BoardPower.h"
#include "log.h"
#include "loop_stats.h"
#include "ConfigStore.h"
#include "MspFcLink.h"
#include "SimRadio.h"
#include "config.h"
#include "crypto.h"
#include "gnss_link.h"
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
    IGnssLink* gnss = nullptr;  // null unless a GPS is wired straight to this node
    const char* fw_version = "dev";
    uint32_t uid = 0;
    uint8_t wifi_channel = 1;
    BoardPower* power = nullptr;  // null on boards with nothing to report
    LoopStats* loop_stats = nullptr;
    // Called after a successful config POST so the caller can push whatever can
    // be changed without a reboot into the live objects.
    void (*on_config_applied)(const Settings&) = nullptr;
};

// Brings WiFi up in the mode the config asks for, and returns the channel the
// radio ended up on.
//
// This MUST run before RadioEspNow::begin(), and nothing may call WiFi.mode()
// afterwards. ESP-NOW binds to the AP interface, so a later mode change tears
// its interface out from under it and the 2.4 GHz link dies silently, with the
// LoRa radio still working and nothing in the counters to explain it. Station
// mode therefore comes up as AP+STA rather than STA: the AP interface has to
// survive for ESP-NOW to exist at all.
//
// The channel matters just as much. ESP-NOW rides whatever channel the WiFi
// radio is on, so two nodes on different channels cannot hear each other even
// though both look healthy. wifi.channel pins it; joining an external network
// overrides it, because the router owns the channel then -- which is exactly
// why joining one is a poor idea on an aircraft that needs ESP-NOW.
uint8_t wifiBringUp(const Settings& cfg, uint32_t uid);

// The channel the radio is actually on, for the status view.
uint8_t wifiChannel();

// ESPAsyncWebServer calls upload handlers as plain functions, so these cannot be
// members; they reach the server through WebServer::instance().
void handleFileUploadData(AsyncWebServerRequest* request, const String& filename, size_t index,
                          uint8_t* data, size_t len, bool final);
void handleFileUploadResponse(AsyncWebServerRequest* request);

// The current Update library error, spelled the way this platform spells it.
String updateErrorText();

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

    // Records a failed upload and, critically, clears the in-progress flag.
    // Leaving it set would keep loop() parked with the radios held still for
    // good: a mistyped filename would take the node off the air until a power
    // cycle, which is a far worse outcome than the failed update itself.
    void failOta(uint16_t code, const String& message) {
        ota_status_ = code;
        ota_message_ = message;
        ota_active_ = false;
        if (Update.isRunning()) {
            Update.end(false);
        }
        // The browser gets this too, but the browser is often a phone that has
        // already navigated away by the time the node answers.
        FF_LOGE("OTA failed: %s", message.c_str());
    }
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
