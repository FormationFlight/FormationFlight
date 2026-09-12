#pragma once
//
// Shared fakes and driving helpers for the Follow controller suite.
//
// The controller is exercised exactly as the firmware wires it: a real
// ff::PeerTable (fed with the same PositionPacket/AnnouncePacket updates the
// radio path produces), an ILocationSource fake for our own fix, and an
// IFollowFc fake that records every outbound FC call. Time is a test-owned
// clock passed into service()/status(), so nothing here races a wall clock.
//
#include <unity.h>

#include <cmath>
#include <cstring>
#include <map>
#include <vector>

#include "follow.h"
#include "geo.h"
#include "peer_table.h"
#include "protocol.h"

using namespace ff;

struct SentWaypoint {
    int32_t lat_1e7;
    int32_t lon_1e7;
    int32_t alt_cm;
    int16_t headingDeg;
};

struct SentGvar {
    uint8_t index;
    int32_t value;
};

// Test double for IFollowFc. Every outbound call is recorded (not just applied)
// so tests can assert both that something was sent and what.
struct FakeFc : IFollowFc {
    bool armedState = false;  // false = disarmed
    bool gcsNav = true;
    bool headingHold = true;
    int32_t altitudeCm = 0;
    FcPlatform platform = FcPlatform::Multirotor;

    // channel (1-based) -> pulse width us. An absent key simulates
    // rcChannelUs() returning false ("no FC connected / out of range"), the
    // one case resolveAxisOffset() falls back to the configured value for.
    std::map<uint8_t, uint16_t> rc;

    std::vector<SentWaypoint> sentWaypoints;
    std::vector<int16_t> sentHeadings;
    std::vector<SentGvar> sentGvars;

    bool needAltitude = false;
    bool needRc = false;

    bool armed() override { return armedState; }
    bool gcsNavActive() override { return gcsNav; }
    bool headingHoldActive() override { return headingHold; }
    bool rcChannelUs(uint8_t channel1Based, uint16_t* out_us) override {
        auto it = rc.find(channel1Based);
        if (it == rc.end()) return false;
        *out_us = it->second;
        return true;
    }
    int32_t localAltitudeCm() override { return altitudeCm; }
    FcPlatform platformType() override { return platform; }
    void sendFollowWaypoint(int32_t lat_1e7, int32_t lon_1e7, int32_t alt_cm,
                            int16_t heading_deg) override {
        sentWaypoints.push_back({lat_1e7, lon_1e7, alt_cm, heading_deg});
    }
    void sendSetHead(int16_t heading_deg) override { sentHeadings.push_back(heading_deg); }
    void sendGvar(uint8_t index, int32_t value) override { sentGvars.push_back({index, value}); }
    void setTelemetryNeeds(bool altitude, bool rcNeeded) override {
        needAltitude = altitude;
        needRc = rcNeeded;
    }
};

// Our own position. Invalid until set().
struct FakeSelf : ILocationSource {
    NodeLocation loc;
    NodeLocation getLocation() override { return loc; }
    void set(double lat, double lon, int16_t alt_m = 0) {
        loc.valid = true;
        loc.lat = static_cast<int32_t>(std::lround(lat * 1e7));
        loc.lon = static_cast<int32_t>(std::lround(lon * 1e7));
        loc.alt_m = alt_m;
    }
};

// Everything a full-cycle test needs, wired the way main.cpp wires it.
struct FollowHarness {
    FakeFc fc;
    FakeSelf self;
    // The table's own expiry is irrelevant here (Follow applies its own
    // peerTimeoutMs to last_position_ms); keep it large so it never interferes.
    PeerTable peers{1000000};
    uint32_t now = 100000;  // start well past 0 so age arithmetic is unambiguous
    FollowController ctl{&peers, &self, &fc};

    // Advances time past the default 4 Hz period (250 ms) and runs one cycle,
    // so the controller's own rate gate never swallows the tick.
    void tick() {
        now += 300;
        ctl.service(now);
    }

    FollowStatus status() const { return ctl.status(now); }

    // Injects/refreshes peer `uid` with a position fix at the current time.
    // lat/lon are degrees, speed m/s, course degrees, alt metres MSL.
    void setPeer(uint32_t uid, double lat, double lon, double speedMs = 0.0,
                 double courseDeg = 0.0, int16_t alt_m = 0) {
        setPeerAt(uid, lat, lon, speedMs, courseDeg, alt_m, now);
    }

    void setPeerAt(uint32_t uid, double lat, double lon, double speedMs, double courseDeg,
                   int16_t alt_m, uint32_t at_ms) {
        PositionPacket p{};
        p.uid = uid;
        p.lat = static_cast<int32_t>(std::lround(lat * 1e7));
        p.lon = static_cast<int32_t>(std::lround(lon * 1e7));
        p.alt_m = alt_m;
        p.speed_cms = static_cast<uint16_t>(std::lround(speedMs * 100.0));
        p.course_ddeg = static_cast<uint16_t>(std::lround(courseDeg * 10.0));
        p.flags = POSITION_FLAG_HAS_FIX;
        peers.updatePosition(p, at_ms, -50, 0);
    }

    // Gives an existing peer a craft name (an announce, not a position).
    void namePeer(uint32_t uid, const char* name) {
        AnnouncePacket a{};
        a.uid = uid;
        std::strncpy(a.name, name, kMaxNameLen);
        a.capabilities = CAP_HAS_GPS;
        peers.updateAnnounce(a, now, 0);
    }

    // Makes peer `uid`'s last position older than any peerTimeoutMs by
    // re-stamping its current position far in the past.
    void markStale(uint32_t uid) {
        const Peer* p = peers.find(uid);
        TEST_ASSERT_NOT_NULL_MESSAGE(p, "markStale: unknown peer");
        PositionPacket pkt{};
        pkt.uid = p->uid;
        pkt.lat = p->lat;
        pkt.lon = p->lon;
        pkt.alt_m = p->alt_m;
        pkt.speed_cms = p->speed_cms;
        pkt.course_ddeg = p->course_ddeg;
        pkt.flags = p->flags;
        peers.updatePosition(pkt, now - 50000, 0, 0);
    }

    // Peer `uid` at (lat, lon) moving at speedMs along courseDeg, with our own
    // fix at the same point (target stays well inside maxTargetDistM).
    void setupLockedPeer(uint32_t uid, double lat, double lon, double speedMs = 10.0,
                         double courseDeg = 0.0) {
        setPeer(uid, lat, lon, speedMs, courseDeg);
        self.set(lat, lon);
    }

    bool apply(const FollowConfig& cfg, const char** err = nullptr) {
        return ctl.applyConfig(cfg, err);
    }
};

// Sugar for the common "start from the live config, tweak, apply" pattern.
inline FollowConfig configOf(const FollowHarness& h) { return h.ctl.config(); }
