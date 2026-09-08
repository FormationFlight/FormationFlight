#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "MSP.h"
#include "../GNSS/GNSSManager.h"
#include "../Peers/PeerManager.h"

#define HOST_MSP_TIMEOUT 8500

enum MSPHost {
    HOST_NONE = 0,
    HOST_GCS = 1,
    HOST_INAV = 2,
    HOST_ARDU = 3,
    HOST_BTFL = 4
};

class MSPManager {
public:
    MSPManager();
    uint8_t getState();
    void getName(char *name, size_t length);
    MSPHost getFCVariant();
    static bool hostIsFlightController(MSPHost host);
    // Returns the connected FC's mixer platform type (MSP2_INAV_MIXER).
    // Cached once a valid reply is received; retried on every call until then (no
    // sys.phase-based give-up, since — unlike getFCVariant() — this is never
    // polled during MODE_HOST_SCAN). Returns INAV_PLATFORM_UNKNOWN until a
    // real reply is received, distinguishable from a genuine
    // INAV_PLATFORM_MULTIROTOR (0) reply; either way it's != INAV_PLATFORM_AIRPLANE,
    // so an unanswered/pre-connection query still fails closed for the
    // autothrottle gate that's this function's only caller.
    InavPlatformType getPlatformType();
    msp_fc_version_t getFCVersion();
    msp_raw_gps_t getLocation();
    msp_analog_t getAnalogValues();
    // Home/baro-relative altitude estimate from the FC, in centimeters (MSP_ALTITUDE).
    // Distinct from getLocation().alt, which is GPS/MSL altitude in meters.
    int32_t local_altitude_cm();
    // Whether the FC currently has GCS NAV active (MSP_MODE_GCSNAV), e.g. for follow-mode gating.
    bool isGCSNavActive();
    // Reads a single RC channel's value (µs) from the FC over a cached MSP_RC
    // poll. channel1Based is 1-16 (MSP_MAX_SUPPORTED_CHANNELS). Returns false (and
    // leaves *outUs untouched) only if not connected to a flight controller or
    // channel1Based is out of range. A transient poll miss on an otherwise-
    // connected FC is NOT one of those failure cases — it returns the last
    // successfully parsed value from the cache instead, same reasoning
    // local_altitude_cm()'s cache already rides out a single dropped frame.
    bool getRcChannelUs(uint8_t channel1Based, uint16_t *outUs);
    void sendRadar(const peer_t *peer);
    // Sends the INAV follow-me special waypoint #255 via MSP_SET_WP.
    // Requires NAV POSHOLD + GCS NAV active on the follower FC.
    // headingDeg: commanded nose heading in degrees, 1-360, or 0
    // to leave heading untouched this cycle — callers must map a computed
    // heading of exactly 0 to 360 themselves (FollowManager does). Kept as a
    // best-effort, forward-compatible write: verified against current INAV
    // firmware source that NAV_STATE_POSHOLD_3D_IN_PROGRESS (the nav state
    // this requires) lacks NAV_REQUIRE_MAGHOLD, so INAV's yaw-rate PID never
    // actually consumes this p1 write today (see sendSetHead()'s comment for
    // the full mechanism) — it's a currently-inert no-op on the FC, not a
    // bug on our end. Left in place (rather than hardcoded to 0) so that if
    // INAV ever extends POSHOLD_3D to honor it — or a follower ends up in
    // some other NAV_REQUIRE_MAGHOLD state — FF already sends the right
    // value with no code change needed.
    void sendFollowWaypoint(int32_t lat_1e7, int32_t lon_1e7, int32_t alt_cm, int16_t headingDeg);
    // Explicitly sets INAV's heading-hold target via MSP_SET_HEAD, decoupled
    // from the WP#255 position stream above and, today, the
    // *only* path that actually works for a follower in NAV POSHOLD_3D.
    // Unlike WP#255's p1, this command has no gating in INAV's handler — it
    // always writes — but the yaw-rate PID only ever reads that target when
    // getHeadingHoldState() reports HEADING_HOLD_ENABLED, which for a
    // follower sitting in NAV POSHOLD_3D requires INAV's own HEADING HOLD
    // ("MAG") box to be active (see isHeadingHoldActive()). Callers must gate
    // on that themselves; sending this with the box inactive is a silent
    // no-op on the FC, not an error. headingDeg: degrees, any value (no
    // (0,360) exclusive restriction the way WP#255's p1 had).
    void sendSetHead(int16_t headingDeg);
    // Whether INAV's HEADING HOLD ("MAG") box is active on the connected FC
    // (MSP_MODE_MAG bit of getActiveModes()) — the precondition for
    // sendSetHead()'s target to actually reach the yaw-rate PID while the
    // follower is in NAV POSHOLD_3D (see sendSetHead()'s comment for why
    // WP#255's p1 alone was never sufficient here).
    bool isHeadingHoldActive();
    // Writes a single INAV Global Variable over MSP2_INAV_SET_GVAR. One-way,
    // best-effort, no ACK wait. Silently no-ops if the connected FC isn't
    // INAV 9.0+ — callers don't need to check support themselves.
    void sendGvar(uint8_t index, int32_t value);
    void sendLocation(GNSSLocation location);
    void begin(Stream &stream);
    void statusJson(JsonDocument *doc);
    void scheduleNextAt(unsigned long timestamp);
    void loop();

    static MSPManager* getSingleton();
private:
    MSP *msp = nullptr;
    bool ready = false;
    // Counter indicating how many peer updates have been sent over MSP
    uint32_t peerUpdatesSent = 0;
    // Counter indicating how many GPS positions have been injected into the FC via MSP
    uint32_t gnssUpdatesSent = 0;
    // Next timestamp at which we'll transmit a single peer's Radar MSP message
    unsigned long nextSendTime = 0;
    // Which peer we'll send next
    uint8_t peerIndex = 0;

    // Cached MSP_STATUS+MSP_BOXIDS active-mode bitmap (MSP::getActiveModes()
    // is itself two MSP round trips) shared by getState() and
    // isGCSNavActive() so calling both in the same cycle costs one poll, not two.
    uint32_t getActiveModesCached();

    uint8_t mapFixType2Msp(GNSS_FIX_TYPE fixType);
};