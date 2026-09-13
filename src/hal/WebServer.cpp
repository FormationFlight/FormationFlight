#include "WebServer.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <AsyncJson.h>

#if defined(PLATFORM_ESP8266)
#include <ESP8266WiFi.h>
#include <Updater.h>
#else
#include <Update.h>
#include <WiFi.h>
#endif

#include <cstdio>
#include <cstring>

#include "geo.h"
#include "webcontent.h"

namespace ff {

WebServer* WebServer::instance_ = nullptr;

namespace {

constexpr size_t kStatusJsonCapacity = 6144;
constexpr size_t kConfigJsonCapacity = 4096;
constexpr size_t kFramesJsonCapacity = 4096;

// UIDs go out as 8-char hex strings, never as JSON numbers: a 32-bit value
// round-tripping through a double is fine today and a very confusing bug the
// day someone widens it.
void uidToHex(uint32_t uid, char out[9]) { std::snprintf(out, 9, "%08x", uid); }

uint32_t hexToUid(const char* s) {
    if (s == nullptr) {
        return 0;
    }
    return static_cast<uint32_t>(std::strtoul(s, nullptr, 16));
}

const char* frameResultName(FrameResult r) {
    switch (r) {
        case FrameResult::Tx:
            return "tx";
        case FrameResult::Ok:
            return "ok";
        case FrameResult::Self:
            return "self";
        case FrameResult::CryptoFail:
            return "crypto_fail";
        case FrameResult::ReplayFail:
            return "replay_fail";
        case FrameResult::DecodeFail:
            return "decode_fail";
        case FrameResult::Oversize:
            return "oversize";
    }
    return "unknown";
}

const char* simModeName(SimMode m) {
    switch (m) {
        case SimMode::Static:
            return "static";
        case SimMode::Line:
            return "line";
        case SimMode::Circle:
            return "circle";
        case SimMode::Hex:
            return "hex";
    }
    return "static";
}

bool simModeFromName(const char* s, SimMode& out) {
    if (s == nullptr) {
        return false;
    }
    if (std::strcmp(s, "static") == 0) {
        out = SimMode::Static;
    } else if (std::strcmp(s, "line") == 0) {
        out = SimMode::Line;
    } else if (std::strcmp(s, "circle") == 0) {
        out = SimMode::Circle;
    } else if (std::strcmp(s, "hex") == 0) {
        out = SimMode::Hex;
    } else {
        return false;
    }
    return true;
}

void sendJson(AsyncWebServerRequest* request, JsonDocument& doc, int code = 200) {
    AsyncResponseStream* response = request->beginResponseStream("application/json");
    response->setCode(code);
    serializeJson(doc, *response);
    request->send(response);
}

double deg1e7(int32_t v) { return static_cast<double>(v) / 1e7; }

}  // namespace

// The Arduino cores spell this differently on each platform. Returns by value:
// the ESP8266 core hands back a String temporary, and taking c_str() off it
// leaves a pointer to freed memory as soon as the expression ends.
String updateErrorText() {
#if defined(PLATFORM_ESP8266)
    return Update.getErrorString();
#else
    return String(Update.errorString());
#endif
}

uint8_t wifiBringUp(const Settings& cfg, uint32_t uid) {
    char ap_ssid[40];
    // The AP name carries the UID so several nodes on a bench can be told apart
    // without connecting to each in turn.
    std::snprintf(ap_ssid, sizeof(ap_ssid), "FormationFlight-%08x",
                  static_cast<unsigned>(uid));

    const bool join = !cfg.wifi.ap && cfg.wifi.ssid[0] != '\0';

    // AP+STA when joining, never plain STA: ESP-NOW lives on the AP interface,
    // and dropping it takes the 2.4 GHz link down with it.
    WiFi.mode(join ? WIFI_AP_STA : WIFI_AP);

    const char* psk = cfg.wifi.ap_psk[0] != '\0' ? cfg.wifi.ap_psk : nullptr;
    WiFi.softAP(ap_ssid, psk, cfg.wifi.channel);

    if (join) {
        WiFi.begin(cfg.wifi.ssid, cfg.wifi.psk);
        // Deliberately does not block: a node whose home network is out of range
        // must still boot, beacon and fly. The association completes in the
        // background, or it does not, and either way the radio work runs.
    }
    return wifiChannel();
}

uint8_t wifiChannel() {
#if defined(PLATFORM_ESP8266)
    return static_cast<uint8_t>(wifi_get_channel());
#else
    return static_cast<uint8_t>(WiFi.channel());
#endif
}

void WebServer::begin(const WebDeps& deps) {
    instance_ = this;
    deps_ = deps;
    // WiFi is already up: main() brings it up before the radios, because ESP-NOW
    // binds to the interface this would otherwise reconfigure.
    server_ = new AsyncWebServer(80);
    registerRoutes();
    server_->begin();
}

void WebServer::loop(uint32_t now_ms) {
    if (reboot_at_ms_ != 0 && static_cast<int32_t>(now_ms - reboot_at_ms_) >= 0) {
        ESP.restart();
    }
}

// ---- Handlers ----------------------------------------------------------------

namespace {

void fillStatus(WebDeps& d, JsonObject root) {
    char hex[9];

    JsonObject node = root.createNestedObject("node");
    uidToHex(d.uid, hex);
    node["uid"] = hex;
    node["name"] = d.cfg->node.name;
    node["version"] = d.fw_version;
    node["uptime_ms"] = millis();
    node["free_heap"] = ESP.getFreeHeap();
    node["listen_only"] = d.cfg->node.listen_only;

    NodeLocation self{};
    if (d.location != nullptr) {
        self = d.location->getLocation();
    }
    JsonObject loc = root.createNestedObject("location");
    loc["valid"] = self.valid;
#ifdef GNSS_ENABLED
    loc["source"] = "gnss";
#else
    loc["source"] = "msp";
#endif
    loc["lat"] = self.lat;
    loc["lon"] = self.lon;
    loc["alt_m"] = self.alt_m;
    loc["speed_cms"] = self.speed_cms;
    loc["course_ddeg"] = self.course_ddeg;
    loc["armed"] = self.armed;

    JsonArray radios = root.createNestedArray("radios");
    const uint32_t now = millis();
    for (size_t i = 0; i < d.node->radioCount(); i++) {
        const RadioStats& rs = d.node->radioStats(i);
        RadioDriver* drv = d.hub->at(i);
        JsonObject r = radios.createNestedObject();
        r["index"] = i;
        r["name"] = drv != nullptr ? drv->name() : "?";
        r["enabled"] = drv != nullptr && drv->enabled();
        r["sim"] = drv != nullptr && std::strcmp(drv->name(), "SIM") == 0;
        r["tx"] = rs.tx;
        r["rx_ok"] = rs.rx_ok;
        r["rx_crypto_fail"] = rs.rx_crypto_fail;
        r["rx_replay"] = rs.rx_replay;
        r["rx_decode_fail"] = rs.rx_decode_fail;
        r["rx_self"] = rs.rx_self;
        r["last_rssi"] = rs.last_rssi;
        r["last_rx_age_ms"] = rs.last_rx_ms == 0 ? 0 : (now - rs.last_rx_ms);
        r["beacon_interval_ms"] = rs.beacon_interval_ms;
        r["airtime_ms"] = rs.airtime_ms;
        r["peers"] = d.node->activePeerCountOn(i, now);
        // Frames lost inside the driver rather than on the air. Invisible in
        // every other counter, and the first sign a node is over its budget.
        r["rx_dropped"] = drv != nullptr ? drv->rxDropped() : 0;
        r["tx_dropped"] = drv != nullptr ? drv->txDropped() : 0;
    }

    JsonArray peers = root.createNestedArray("peers");
    const PeerTable& table = d.node->peers();
    for (size_t i = 0; i < table.capacity(); i++) {
        const Peer* p = table.at(i);
        if (p == nullptr) {
            continue;
        }
        JsonObject o = peers.createNestedObject();
        uidToHex(p->uid, hex);
        o["uid"] = hex;
        o["name"] = p->name;
        o["lat"] = p->lat;
        o["lon"] = p->lon;
        o["alt_m"] = p->alt_m;
        o["speed_cms"] = p->speed_cms;
        o["course_ddeg"] = p->course_ddeg;
        o["flags"] = p->flags;
        o["rssi"] = p->rssi;
        o["age_ms"] = now - p->last_update_ms;
        o["packets"] = p->packets_received;
        JsonArray on = o.createNestedArray("radios");
        for (size_t r = 0; r < kMaxRadios; r++) {
            if (p->radios_seen & (1u << r)) {
                on.add(r);
            }
        }
        // Range and bearing are only meaningful with our own fix; omit rather
        // than send a distance measured from latitude zero.
        if (self.valid) {
            o["distance_m"] = geo::distanceM(deg1e7(self.lat), deg1e7(self.lon), deg1e7(p->lat),
                                             deg1e7(p->lon));
            o["bearing_deg"] = geo::bearingDeg(deg1e7(self.lat), deg1e7(self.lon),
                                               deg1e7(p->lat), deg1e7(p->lon));
            o["rel_alt_m"] = p->alt_m - self.alt_m;
        }
    }

    JsonObject crypto = root.createNestedObject("crypto");
    if (d.crypto != nullptr) {
        crypto["mode"] = "ccm";
        crypto["bad_tag"] = d.crypto->badTagCount();
        crypto["replay"] = d.crypto->replayCount();
        crypto["tx_counter"] = d.crypto->txCounter();
    } else {
        crypto["mode"] = "none";
    }

    const NodeStats& st = d.node->stats();
    JsonObject stats = root.createNestedObject("stats");
    stats["beacons_sent"] = st.beacons_sent;
    stats["announces_sent"] = st.announces_sent;
    stats["rx_ok"] = st.rx_ok;
    stats["rx_rejected"] = st.rx_rejected;
    stats["rx_self"] = st.rx_self;

    JsonObject fc = root.createNestedObject("fc");
    if (d.fc != nullptr) {
        fc["connected"] = d.fc->connected();
        fc["variant"] = d.fc->variant();
        char ver[16];
        const FcVersion v = d.fc->version();
        std::snprintf(ver, sizeof(ver), "%u.%u.%u", v.major, v.minor, v.patch);
        fc["version"] = ver;
        fc["platform"] = static_cast<int>(d.fc->platformType());
        fc["armed"] = d.fc->armed();
        fc["gcs_nav"] = d.fc->gcsNavActive();
        fc["heading_hold"] = d.fc->headingHoldActive();
    } else {
        fc["connected"] = false;
    }

    if (d.follow != nullptr) {
        const FollowStatus fs = d.follow->status(now);
        JsonObject f = root.createNestedObject("follow");
        f["state"] = followLockStateName(fs.state);
        f["gate_active"] = fs.gateActive;
        uidToHex(fs.lockedUid, hex);
        f["locked_uid"] = hex;
        f["locked_name"] = fs.lockedName;
        f["platform"] = static_cast<int>(fs.platformType);
        f["autothrottle_armed"] = fs.autothrottleArmed;
        f["rc_slot_frozen"] = fs.rcSlotFrozen;
        f["prearm_failed"] = fs.rcPreArmCheckFailed;
        if (fs.haveLastTarget) {
            JsonObject t = f.createNestedObject("target");
            t["lat"] = fs.lastTarget.lat_1e7;
            t["lon"] = fs.lastTarget.lon_1e7;
            t["alt_cm"] = fs.lastTargetAltCm;
            t["heading_deg"] = fs.lastTargetHeadingDeg;
            t["age_ms"] = fs.lastTargetAgeMs;
            JsonObject lo = f.createNestedObject("live_offset");
            lo["long_m"] = fs.liveOffset.longitudinal_m;
            lo["lat_m"] = fs.liveOffset.lateral_m;
            lo["vert_m"] = fs.liveOffset.vertical_m;
            f["autothrottle_engaged"] = fs.autothrottleEngaged;
            f["target_speed_cms"] = fs.targetSpeedCmS;
        }
        if (fs.haveStatusGvarValue) {
            f["status_gvar"] = fs.statusGvarValue;
        }
        if (fs.haveConditionFlagsGvarValue) {
            f["condition_gvar"] = fs.conditionFlagsGvarValue;
        }
        if (fs.havePreArmCandidateOffset) {
            JsonObject po = f.createNestedObject("prearm_offset");
            po["long_m"] = fs.preArmCandidateOffset.longitudinal_m;
            po["lat_m"] = fs.preArmCandidateOffset.lateral_m;
            po["vert_m"] = fs.preArmCandidateOffset.vertical_m;
        }
    }

    JsonObject sim = root.createNestedObject("sim");
    sim["enabled"] = d.cfg->sim.enabled;
    sim["peers"] = d.sim != nullptr ? d.sim->peerCount() : 0;

    JsonObject wifi = root.createNestedObject("wifi");
    wifi["mode"] = d.cfg->wifi.ap ? "ap" : "ap_sta";
    // The channel ESP-NOW is actually on. Two nodes on different channels cannot
    // hear each other over ESP-NOW however healthy both look, and joining an
    // external network hands the choice to the router, so this is worth showing.
    wifi["channel"] = wifiChannel();
    wifi["configured_channel"] = d.cfg->wifi.channel;
    wifi["ap_clients"] = WiFi.softAPgetStationNum();
    if (!d.cfg->wifi.ap) {
        wifi["sta_connected"] = WiFi.status() == WL_CONNECTED;
        wifi["sta_rssi"] = WiFi.RSSI();
    }

    root["reboot_required"] = WebServer::instance()->rebootRequired();
    root["config_corrupt"] = d.store != nullptr && d.store->lastLoadCorrupt();
}

}  // namespace

void WebServer::registerRoutes() {
    AsyncWebServer* s = server_;
    // staticfilehandler.inc, generated by scripts/build_html.py from html/,
    // registers its routes against a variable called `server`.
    AsyncWebServer* server = server_;

    s->on("/api/status", HTTP_GET, [](AsyncWebServerRequest* request) {
        DynamicJsonDocument doc(kStatusJsonCapacity);
        JsonObject root = doc.to<JsonObject>();
        fillStatus(WebServer::instance()->deps(), root);
        sendJson(request, doc);
    });

    s->on("/api/config", HTTP_GET, [](AsyncWebServerRequest* request) {
        DynamicJsonDocument doc(kConfigJsonCapacity);
        JsonObject root = doc.to<JsonObject>();
        configToJson(*WebServer::instance()->deps().cfg, root, /*redact_secrets=*/true);
        sendJson(request, doc);
    });

    // Partial config update. The merge and every validation rule live in
    // ff_core/config.cpp, shared with the host tests, so this handler is only
    // plumbing.
    AsyncCallbackJsonWebHandler* configPost = new AsyncCallbackJsonWebHandler(
        "/api/config", [](AsyncWebServerRequest* request, JsonVariant& json) {
            WebDeps& d = WebServer::instance()->deps();
            if (!json.is<JsonObject>()) {
                request->send(400, "text/plain", "body must be a JSON object");
                return;
            }
            const char* err = nullptr;
            if (!configMergeJson(json.as<JsonObjectConst>(), *d.cfg, &err)) {
                request->send(400, "text/plain", err != nullptr ? err : "invalid config");
                return;
            }
            if (d.on_config_applied != nullptr) {
                d.on_config_applied(*d.cfg);
            }
            DynamicJsonDocument doc(kConfigJsonCapacity);
            JsonObject root = doc.to<JsonObject>();
            configToJson(*d.cfg, root, /*redact_secrets=*/true);
            sendJson(request, doc);
        });
    s->addHandler(configPost);

    s->on("/api/config/save", HTTP_POST, [](AsyncWebServerRequest* request) {
        WebServer* w = WebServer::instance();
        WebDeps& d = w->deps();
        if (!w->saveAllowed(millis())) {
            request->send(429, "text/plain", "saved too recently, try again shortly");
            return;
        }
        const char* err = nullptr;
        if (d.store == nullptr || !d.store->save(*d.cfg, &err)) {
            request->send(500, "text/plain", err != nullptr ? err : "no filesystem");
            return;
        }
        w->markSaved(millis());
        request->send(200, "text/plain", "saved");
    });

    s->on("/api/config/reset", HTTP_POST, [](AsyncWebServerRequest* request) {
        WebDeps& d = WebServer::instance()->deps();
        *d.cfg = Settings{};
        if (d.store != nullptr) {
            d.store->save(*d.cfg, nullptr);
        }
        if (d.on_config_applied != nullptr) {
            d.on_config_applied(*d.cfg);
        }
        // Radio and WiFi settings only take effect at boot, and a factory reset
        // almost certainly changed some.
        WebServer::instance()->setRebootRequired();
        request->send(200, "text/plain", "reset");
    });

    s->on("/api/frames", HTTP_GET, [](AsyncWebServerRequest* request) {
        WebDeps& d = WebServer::instance()->deps();
        const FrameLog& log = d.node->frameLog();
        uint32_t since = 0;
        if (request->hasParam("since")) {
            since = static_cast<uint32_t>(request->getParam("since")->value().toInt());
        }

        DynamicJsonDocument doc(kFramesJsonCapacity);
        JsonObject root = doc.to<JsonObject>();
        root["total"] = log.total();
        root["capacity"] = log.capacity();
        JsonArray arr = root.createNestedArray("frames");
        // Entry i counts back from the newest; its sequence number is
        // total - 1 - i. Stop as soon as we reach what the client already has.
        char hex[9];
        for (size_t i = 0; i < log.size(); i++) {
            const uint32_t seq = log.total() - 1 - static_cast<uint32_t>(i);
            if (since != 0 && seq < since) {
                break;
            }
            const FrameLogEntry& e = log.at(i);
            JsonObject o = arr.createNestedObject();
            o["ms"] = e.ms;
            o["radio"] = e.radio;
            uidToHex(e.uid, hex);
            o["uid"] = hex;
            o["type"] = e.type;
            o["len"] = e.len;
            o["rssi"] = e.rssi;
            o["result"] = frameResultName(e.result);
            if (doc.overflowed()) {
                break;
            }
        }
        sendJson(request, doc);
    });

    // ---- Simulated traffic ----------------------------------------------------

    s->on("/api/sim", HTTP_GET, [](AsyncWebServerRequest* request) {
        WebDeps& d = WebServer::instance()->deps();
        DynamicJsonDocument doc(2048);
        JsonObject root = doc.to<JsonObject>();
        root["enabled"] = d.cfg->sim.enabled;
        JsonArray arr = root.createNestedArray("peers");
        if (d.sim != nullptr) {
            const uint32_t now = millis();
            char hex[9];
            for (size_t i = 0; i < d.sim->peerCount(); i++) {
                const SimPeerConfig* c = d.sim->peerAt(i);
                JsonObject o = arr.createNestedObject();
                uidToHex(c->uid, hex);
                o["uid"] = hex;
                o["name"] = c->name;
                o["mode"] = simModeName(c->mode);
                o["lat"] = c->lat;
                o["lon"] = c->lon;
                o["alt_m"] = c->alt_m;
                o["speed_ms"] = c->speed_ms;
                o["course_deg"] = c->course_deg;
                o["radius_m"] = c->radius_m;
                o["elapsed_ms"] = d.sim->peerElapsedMs(i, now);
                o["running"] = d.cfg->sim.enabled;
            }
        }
        sendJson(request, doc);
    });

    AsyncCallbackJsonWebHandler* simPost = new AsyncCallbackJsonWebHandler(
        "/api/sim/peer", [](AsyncWebServerRequest* request, JsonVariant& json) {
            WebDeps& d = WebServer::instance()->deps();
            if (d.sim == nullptr) {
                request->send(409, "text/plain", "simulator not available on this build");
                return;
            }
            if (!d.cfg->sim.enabled) {
                request->send(409, "text/plain", "set sim.enabled before adding peers");
                return;
            }
            JsonObjectConst o = json.as<JsonObjectConst>();

            SimPeerConfig c;
            c.uid = hexToUid(o["uid"] | "");
            if (c.uid == 0) {
                // Generated UIDs are marked so a simulated peer is recognisable
                // as one even in a raw packet capture.
                c.uid = 0x5EED0000u | static_cast<uint32_t>(d.sim->peerCount() + 1);
            }
            std::strncpy(c.name, o["name"] | "SIM", kMaxNameLen);
            c.name[kMaxNameLen] = '\0';
            if (!simModeFromName(o["mode"] | "static", c.mode)) {
                request->send(400, "text/plain", "mode must be static, line, circle or hex");
                return;
            }
            c.lat = o["lat"] | 0.0;
            c.lon = o["lon"] | 0.0;
            c.alt_m = static_cast<int16_t>(o["alt_m"] | 100);
            c.speed_ms = o["speed_ms"] | 0.0;
            c.course_deg = o["course_deg"] | 0.0;
            c.radius_m = o["radius_m"] | 100.0;

            if (c.speed_ms < 0.0 || c.speed_ms > 200.0) {
                request->send(400, "text/plain", "speed_ms must be 0-200");
                return;
            }
            if ((c.mode == SimMode::Circle || c.mode == SimMode::Hex) && c.radius_m <= 0.0) {
                request->send(400, "text/plain", "radius_m must be > 0 for circle and hex");
                return;
            }
            if (!d.sim->setPeer(c, millis())) {
                request->send(409, "text/plain", "simulated peer table full");
                return;
            }
            request->send(200, "text/plain", "ok");
        });
    s->addHandler(simPost);

    s->on("/api/sim/peer", HTTP_DELETE, [](AsyncWebServerRequest* request) {
        WebDeps& d = WebServer::instance()->deps();
        if (d.sim == nullptr || !request->hasParam("uid")) {
            request->send(400, "text/plain", "uid required");
            return;
        }
        const uint32_t uid = hexToUid(request->getParam("uid")->value().c_str());
        const bool removed = d.sim->removePeer(uid);
        request->send(removed ? 200 : 404, "text/plain", removed ? "removed" : "no such peer");
    });

    s->on("/api/sim/clear", HTTP_POST, [](AsyncWebServerRequest* request) {
        WebDeps& d = WebServer::instance()->deps();
        if (d.sim != nullptr) {
            d.sim->clear();
        }
        request->send(200, "text/plain", "cleared");
    });

    s->on("/api/system/reboot", HTTP_POST, [](AsyncWebServerRequest* request) {
        // Reboot after the response has had time to go out; rebooting inside the
        // handler drops the connection and the UI reports a failure.
        WebServer::instance()->requestReboot(millis() + 500);
        request->send(200, "text/plain", "rebooting");
    });

    s->on("/update", HTTP_POST, handleFileUploadResponse, handleFileUploadData);

    s->onNotFound([](AsyncWebServerRequest* request) {
        if (request->method() == HTTP_OPTIONS) {
            request->send(200);  // CORS preflight, for UI development off-device
        } else {
            request->send(404, "text/plain", "Not found");
        }
    });

    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET,POST,DELETE,OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");

#include "staticfilehandler.inc"
}

// ---- Firmware upload ---------------------------------------------------------

// ---- Firmware upload ---------------------------------------------------------
//
// ESPAsyncWebServer streams the body to handleFileUploadData in chunks and then
// calls handleFileUploadResponse once, so the outcome has to be carried between
// them on the server object.

void handleFileUploadData(AsyncWebServerRequest* request, const String& filename, size_t index,
                          uint8_t* data, size_t len, bool final) {
    WebServer* w = WebServer::instance();

    if (index == 0) {
        // A new upload. Clear whatever the previous one left behind, or a retry
        // after a failure reports the old error and never even starts.
        w->otaStatus() = 0;
        w->otaMessage() = "";
    }
    if (w->otaStatus() != 0) {
        return;  // already failed; swallow the rest of the body
    }

#if defined(PLATFORM_ESP8266)
    const bool named_ok = filename.endsWith(".bin") || filename.endsWith(".bin.gz");
    const char* want = "must upload .bin or .bin.gz";
#else
    const bool named_ok = filename.endsWith(".bin");
    const char* want = "must upload .bin";
#endif
    if (!named_ok) {
        // Checked before a single byte reaches flash: uploading the wrong file
        // and finding out after the erase is a brick, not an error message.
        w->failOta(400, want);
        return;
    }

    if (index == 0) {
        if (Update.isRunning()) {
            Update.end(false);
        }
#if defined(PLATFORM_ESP8266)
        Update.runAsync(true);
#endif
        if (!Update.begin(request->contentLength(), U_FLASH)) {
            w->failOta(500, updateErrorText());
            return;
        }
        w->setOtaActive();
    }

    if (Update.write(data, len) != len) {
        w->failOta(500, updateErrorText());
        return;
    }

    if (final) {
        if (!Update.end(true)) {
            w->failOta(500, updateErrorText());
            return;
        }
        w->otaMessage() = "update complete, rebooting";
        w->otaStatus() = 200;
    }
}

void handleFileUploadResponse(AsyncWebServerRequest* request) {
    WebServer* w = WebServer::instance();
    const uint16_t code = w->otaStatus() != 0 ? w->otaStatus() : 500;
    const String msg = w->otaMessage().length() > 0 ? w->otaMessage() : String("upload failed");
    request->send(code, "text/plain", msg);
    if (code == 200) {
        w->requestReboot(millis() + 500);
    }
}

}  // namespace ff
