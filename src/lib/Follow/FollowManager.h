#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "FollowConfig.h"
#include "../Peers/PeerManager.h"
#include "FollowDeps.h"

enum FollowLockState {
    FOLLOW_LOCK_IDLE = 0,
    FOLLOW_LOCK_ACQUIRING = 1,
    FOLLOW_LOCK_LOCKED = 2,
    FOLLOW_LOCK_LOCKED_HOLDING = 3,
};

// Condition-code table sent via conditionFlagsGvarIndex. Sequential, not a
// bitmask — only one code is ever reported at a time. Values are ordered
// low-to-high by pilot-relevant priority (see raiseCondition() in
// FollowManager.cpp's loop()): when multiple conditions are true in the same
// cycle, the highest-valued one wins. A renumber would break any pilot's
// INAV Logic Condition already wired against the old number, so prefer
// appending future conditions above FOLLOW_CONDITION_RC_INVALID_GAP_SETTINGS
// in priority order rather than renumbering an existing value; only insert
// mid-range when priority genuinely demands it.
enum FollowConditionCode {
    FOLLOW_CONDITION_NONE = 0,                    // no condition active
    FOLLOW_CONDITION_FLOOR_CLAMPED = 1,            // altitude floor clamped, unrelated to RC
    FOLLOW_CONDITION_TARGET_TOO_FAR = 2,           // solved target beyond maxTargetDistM; waypoint suppressed
    FOLLOW_CONDITION_RC_INVALID_GAP_SETTINGS = 3,  // RC-attributable invalid gap settings, and/or rcSlotFrozen
};

// Resolved 3D slot offset in the leader's track-relative frame, meters.
// Kept as double (not float) since it's combined with GPS-derived doubles
// and FC altitude centimeters in FollowManager.cpp's target math.
struct FollowOffset {
    double longitudinal_m; // +ahead / -behind
    double lateral_m;      // +right / -left
    double vertical_m;     // +above / -below
};

struct FollowTarget {
    int32_t lat_1e7;
    int32_t lon_1e7;
};

// Runtime-editable mirror of every compile-time #define in FollowConfig.h
// except FOLLOW_TRIGGER_MODE/FOLLOW_AUX_* (AUX-channel triggering isn't
// implemented — trigger mode stays compile-time-only and is reported
// read-only in configJson()). Seeded from FollowConfig.h's #defines at
// construction, mutated in place by WiFiManager's POST
// /followmanager/config, and persisted to EEPROM on demand (see
// FollowEepromRecord/loadFromEEPROM()/saveToEEPROM() below).
struct FollowRuntimeConfig {
    // Canonical track-relative offset, meters. The
    // AHEAD/BEHIND/LEFT/RIGHT/ABOVE/BELOW "friendly grid" some UIs present
    // is purely a client-side view over these signed values (html/follow.js)
    // — FollowManager itself only ever deals in this one representation.
    double ofsLongM = FOLLOW_OFS_LONG_M; // +ahead / -behind
    double ofsLatM = FOLLOW_OFS_LAT_M;   // +right / -left
    double ofsVertM = FOLLOW_OFS_VERT_M; // +above / -below

    // 0 = FIRST_ACTIVE, nonzero = pin to that peer id.
    uint8_t targetPeer = FOLLOW_TARGET_PEER;
    uint16_t emitHz = FOLLOW_EMIT_HZ;
    uint32_t peerTimeoutMs = FOLLOW_PEER_TIMEOUT_MS;

    double minSepM = FOLLOW_MIN_SEP_M;
    double minVSepM = FOLLOW_MIN_VSEP_M;
    double maxTargetDistM = FOLLOW_MAX_TARGET_DIST_M;
    double minAltM = FOLLOW_MIN_ALT_M;

    double minCourseSpeed = FOLLOW_MIN_COURSE_SPEED;

    // Commanded nose heading — sent via WP#255's p1, not a
    // second MSP message. headingDeg's meaning depends on headingMode (see
    // FollowConfig.h's FollowHeadingMode).
    FollowHeadingMode headingMode = FOLLOW_HEADING_MODE;
    double headingDeg = FOLLOW_HEADING_DEG;

    // GVAR indices, -1 = disabled. Range enforced in
    // applyConfig() (-1 or 0-7); the web UI additionally makes an
    // out-of-range value structurally unreachable via a <select>.
    int16_t statusGvarIndex = FOLLOW_STATUS_GVAR_INDEX;
    int16_t conditionFlagsGvarIndex = FOLLOW_CONDITION_FLAGS_GVAR_INDEX;

    // RC axis control: 1-based MSP_RC channel per axis, or -1 =
    // disabled. Range enforced in applyConfig() (-1 or 1-16). The configured
    // ofs{Long,Lat,Vert}M value becomes that axis's live-scaled bound once a
    // channel is assigned, not a fixed point.
    int16_t rcLongChannel = FOLLOW_RC_LONG_CHANNEL;
    int16_t rcLatChannel = FOLLOW_RC_LAT_CHANNEL;
    int16_t rcVertChannel = FOLLOW_RC_VERT_CHANNEL;

    // Speed autothrottle. GVAR indices, -1 = disabled, same convention/range
    // (applyConfig(): -1 or 0-7) as statusGvarIndex/conditionFlagsGvarIndex above.
    int16_t targetSpeedGvarIndex = FOLLOW_TARGET_SPEED_GVAR_INDEX;
    int16_t autothrottleEngageGvarIndex = FOLLOW_AUTOTHROTTLE_ENGAGE_GVAR_INDEX;
    // Pilot's autothrottle arm switch, 1-based MSP_RC channel, or
    // -1 = unassigned (always armed). Armed while the channel's pulse width
    // falls within [autothrottleEnableMinThresholdUs, autothrottleEnableMaxThresholdUs].
    int16_t autothrottleEnableRcChannel = FOLLOW_AUTOTHROTTLE_ENABLE_RC_CHANNEL;
    int16_t autothrottleEnableMinThresholdUs = FOLLOW_AUTOTHROTTLE_ENABLE_MIN_THRESHOLD_US;
    int16_t autothrottleEnableMaxThresholdUs = FOLLOW_AUTOTHROTTLE_ENABLE_MAX_THRESHOLD_US;
    // Slot-lag correction: max closing acceleration/deceleration (cm/s^2)
    // used by the kinematic braking law in resolveTargetSpeedCmS() —
    // v = sqrt(2 * a * d), the speed that lets the follower close an
    // along-track error `d` while still decelerating to exactly match the
    // leader's speed by the time d reaches 0. 0 = feedforward-only (mirror
    // the leader's speed exactly).
    int16_t speedCorrectionAccelCmS2 = FOLLOW_SPEED_CORRECTION_ACCEL_CMS2;
    // m/s clamp bounds for the autothrottle setpoint. minTargetSpeedMps
    // is this feature's only stall-safety mechanism this iteration.
    double minTargetSpeedMps = FOLLOW_MIN_TARGET_SPEED_MPS;
    double maxTargetSpeedMps = FOLLOW_MAX_TARGET_SPEED_MPS;

    // RAM only, deliberately absent from FollowEepromRecord below — always
    // resets to false on reboot rather than persisting (see FollowConfig.h's
    // FOLLOW_DEBUG_ENABLED comment). When true, the follower's own commanded
    // waypoint lat/lon/alt/heading are written to GVARs 0-3 every loop() cycle.
    bool debug = FOLLOW_DEBUG_ENABLED;
};

// EEPROM persistence: FollowRuntimeConfig's on-disk mirror, kept
// as its own versioned record so a fresh/uninitialized EEPROM region (or a
// future struct layout change) is detected independently of cfg's own
// VERSION_CONFIG (main.h) — a version mismatch here just means "nothing
// saved yet," not "corrupt," so FollowManager falls back to the
// compile-time defaults FollowRuntimeConfig's member initializers already
// seeded. Lives immediately after cfg's own EEPROM footprint — see
// ConfigHandler.cpp's config_init(), which sizes EEPROM.begin() to fit both.
//
// FollowRuntimeConfig's geometry/timing fields are `double` because
// FollowManager.cpp's target math combines them with GPS-derived doubles —
// but the web UI can never actually produce a
// fractional value for any of them: every type="number" field in
// html/follow.js goes through html/components.js's TextValue, which calls
// parseInt() on the input before it ever reaches state. So the EEPROM
// mirror stores those fields as int16_t instead of double — a quarter the
// size, with no precision loss for anything the UI can send — and
// FollowManager.cpp's toEepromRecord()/fromEepromRecord() do the
// int16_t<->double conversion in the one place it's needed. (int16_t's
// +-32767 range comfortably covers every field here: offsets/distances in
// meters, speed in m/s, heading in degrees.) targetPeer/emitHz/
// peerTimeoutMs/headingMode are already integer types in
// FollowRuntimeConfig, so they're carried through unchanged, no conversion
// needed.
// Bumped 6: speedCorrectionKp -> speedCorrectionAccelCmS2. Same int16_t
// layout, but the stored value's meaning changed (linear gain -> kinematic
// braking accel), so a stale record must fall back to defaults rather than
// silently reinterpreting an old Kp as an accel.
#define FOLLOW_EEPROM_VERSION 6
struct FollowEepromRecord {
    uint16_t version;

    int16_t ofsLongM;
    int16_t ofsLatM;
    int16_t ofsVertM;

    uint8_t targetPeer;
    uint16_t emitHz;
    uint32_t peerTimeoutMs;

    int16_t minSepM;
    int16_t minVSepM;
    int16_t maxTargetDistM;
    int16_t minAltM;

    int16_t minCourseSpeed;

    FollowHeadingMode headingMode;
    int16_t headingDeg;

    int16_t statusGvarIndex;
    int16_t conditionFlagsGvarIndex;

    int16_t rcLongChannel;
    int16_t rcLatChannel;
    int16_t rcVertChannel;

    int16_t targetSpeedGvarIndex;
    int16_t autothrottleEngageGvarIndex;
    int16_t autothrottleEnableRcChannel;
    int16_t autothrottleEnableMinThresholdUs;
    int16_t autothrottleEnableMaxThresholdUs;
    int16_t speedCorrectionAccelCmS2;
    int16_t minTargetSpeedMps;
    int16_t maxTargetSpeedMps;
};

// Projects a leader position + track-relative offset to an absolute lat/lon.
// peer_lat_1e6/peer_lon_1e6 are peer_t::gps's raw internal representation
// (degrees x 1e6, see PeerManager.h — NOT the x1e7 MSP wire format).
// course_deg is plain degrees, clockwise from north.
FollowTarget slotToLatLon(int32_t peer_lat_1e6, int32_t peer_lon_1e6, double course_deg,
                          double long_m, double lat_m);

class FollowManager
{
public:
    // deps are borrowed pointers, not owned -- production wiring
    // (getSingleton(), FollowProdAdapters.cpp) points them at
    // process-lifetime real adapters; tests point them at fakes with
    // whatever lifetime the test itself manages.
    FollowManager(IFollowMsp *msp, IFollowGnss *gnss, IFollowPeers *peers);
    void loop();
    // Read-only snapshot for GET /followmanager/status: PeerLock
    // state, locked peer id/name (if any), gate active/inactive, and the last
    // computed target (if any target has been solved this session).
    void statusJson(JsonDocument *doc);
    // Resolved runtime config for GET /followmanager/config.
    void configJson(JsonDocument *doc) const;
    const FollowRuntimeConfig &getConfig() const { return config; }
    // Validates newConfig (offset geometry rules + basic field sanity).
    // On success, replaces the active config atomically and returns true;
    // if newConfig's targetPeer differs from the current one, forces a
    // fresh peer acquire (the explicit "user changes a setting" escape
    // hatch — see forceReacquire()). On failure, leaves the active config
    // untouched, returns false, and fills *errMsg.
    bool applyConfig(const FollowRuntimeConfig &newConfig, String *errMsg);
    // Loads the persisted config from EEPROM if present and valid. Called
    // once at startup, after ConfigHandler's config_init() has already
    // called EEPROM.begin(). Leaves the compile-time-seeded config
    // untouched if there's nothing valid to load (fresh flash, version
    // mismatch, or a record that fails applyConfig()'s validation).
    void loadFromEEPROM();
    // Persists the current in-memory config to its EEPROM region — an
    // explicit action distinct from applyConfig()'s RAM-only
    // mutation. Rate-limited so rapid repeated calls (e.g. accidental
    // double-clicks) don't hammer EEPROM; on failure (including
    // rate-limiting) returns false and fills *errMsg.
    bool saveToEEPROM(String *errMsg);
    static FollowManager *getSingleton();

private:
    IFollowMsp *msp;
    IFollowGnss *gnss;
    IFollowPeers *peers;

    FollowRuntimeConfig config;
    unsigned long lastEepromCommitMs = 0;
    FollowLockState state = FOLLOW_LOCK_IDLE;
    uint8_t lockedId = 0;
    char lockedName[NAME_LENGTH + 1] = "";
    double lastValidCourseDeg = 0.0;
    bool haveValidCourse = false;
    unsigned long nextRunTime = 0;

    // Last target actually emitted via sendFollowWaypoint() (i.e. it passed
    // offsetGeometrySane()/targetTooFar()), for status reporting only — not
    // used by the control loop.
    bool haveLastTarget = false;
    FollowTarget lastTarget{};
    int32_t lastTargetAltCm = 0;
    int16_t lastTargetHeadingDeg = 0;
    unsigned long lastTargetTime = 0;

    // Last value actually written to each GVAR (used by the change+heartbeat
    // send rule below — see updateStatusGvars()), or INT32_MIN as a "never
    // sent yet" sentinel so the very first loop() cycle always sends —
    // including an explicit 0 if that's the resting value, with no separate
    // startup-only code path needed.
    int32_t lastSentStatusGvarValue = INT32_MIN;
    int32_t lastSentConditionFlagsGvarValue = INT32_MIN;
    unsigned long lastStatusGvarSendMs = 0;
    unsigned long lastConditionFlagsGvarSendMs = 0;
    int32_t lastSentAutothrottleEngageValue = INT32_MIN;
    unsigned long lastAutothrottleEngageSendMs = 0;

    // Last computed autothrottle setpoint/engage state, for statusJson() —
    // set alongside lastTarget etc. at loop()'s success-path tail.
    int32_t lastTargetSpeedCmS = 0;
    bool lastAutothrottleEngaged = false;

    // "Last known good" RC-scaled offset triple — the freeze target when a
    // candidate fails either safety layer. Bootstrapped to the compile-time
    // static offset, matching FollowRuntimeConfig's own member
    // initializers; applyConfig() resets this to the new config's static
    // offset on every successful apply.
    FollowOffset lastKnownGood{FOLLOW_OFS_LONG_M, FOLLOW_OFS_LAT_M, FOLLOW_OFS_VERT_M};
    // Whether resolveOffset()'s last cycle held lastKnownGood frozen rather
    // than adopting a fresh candidate, for statusJson()/updateStatusGvars().
    bool rcSlotFrozen = false;
    // Ground-only pre-arm advisory result, recomputed every loop() cycle
    // while disarmed and reset to false every cycle otherwise.
    bool rcPreArmCheckFailed = false;
    // The RC-scaled candidate the pre-arm check evaluated this cycle, kept around
    // so statusJson() can report actual numbers (not just pass/fail) for
    // bench-testing RC scaling before arming. Same gating as
    // rcPreArmCheckFailed — havePreArmCandidateOffset false whenever the
    // craft isn't disarmed-with-an-axis-assigned, so a stale value never
    // lingers into a cycle where it wasn't recomputed.
    FollowOffset preArmCandidateOffset{};
    bool havePreArmCandidateOffset = false;
    // Offset triple actually used for the last emitted waypoint, distinct
    // from lastKnownGood which persists even on a
    // geometry-sane/targetTooFar() rejection where no waypoint went out.
    FollowOffset lastLiveOffset{};

    bool followSwitchActive();
    // Advances the PeerLock state machine and returns the peer to
    // track this cycle, or nullptr if we're still acquiring or holding.
    const peer_t *resolveLock();
    // Clears any held/locked peer and drops back to ACQUIRING so the next
    // loop() cycle re-scans under the (possibly just-changed) targetPeer
    // setting — the explicit mechanism that lets a pilot force an immediate
    // peer-target change instead of waiting for the current lock to be
    // lost. Safe to call even when the gate is inactive — loop()
    // unconditionally forces IDLE in that case regardless of what state
    // this leaves behind.
    void forceReacquire();
    // Current config's canonical offset, i.e. {ofsLongM,
    // ofsLatM, ofsVertM} as a FollowOffset, scaled live by any RC-assigned
    // axis and passed through the two-layer geometry safety net.
    FollowOffset resolveOffset();
    // Per-axis RC-to-offset mapping. channel1Based < 1 means
    // "no channel assigned" -> returns configuredM unchanged (the only
    // fallback case). Once a channel is assigned, whatever value comes back
    // is mapped unconditionally, including a raw reading below 1000us -- it
    // just clamps to the 1000 endpoint like any other out-of-range value.
    // getRcChannelUs() returning false (no FC connected, or
    // an out-of-MSP_RC-range channel number) is the one case where there's
    // genuinely no value to map, so that also falls back to configuredM.
    double resolveAxisOffset(double configuredM, int16_t channel1Based) const;
    // {resolveAxisOffset(config.ofs{Long,Lat,Vert}M, config.rc{Long,Lat,Vert}Channel)}
    // as a triple — shared by resolveOffset() and the pre-arm advisory
    // check (rcPreArmCheckFailed) so both build the exact same candidate
    // from the same inputs.
    FollowOffset resolveCandidateOffset() const;
    bool anyRcChannelAssigned() const;
    // Leader's usable track course in plain degrees, applying the
    // low-speed/stationary fallback.
    double resolveCourseDeg(const peer_t *peer);
    // Runtime sanity: is the solved target farther than config.maxTargetDistM
    // from the follower's own position? Geometry sanity
    // (offsetGeometrySane()) is checked separately by the caller so it can
    // attribute a failure here specifically to FOLLOW_CONDITION_TARGET_TOO_FAR.
    bool targetTooFar(const FollowTarget &target) const;
    // Commanded nose heading for this cycle, resolved per config.headingMode.
    // courseDeg is the already-computed resolveCourseDeg() result, reused
    // here instead of recomputed. Returns 0 for
    // FOLLOW_HEADING_OFF (this function's own "don't send a heading" wire
    // sentinel, consumed by loop() before calling MSPManager::sendSetHead() —
    // also passed through to sendFollowWaypoint()'s p1, see that function's
    // comment for why that's currently a forward-compatible no-op rather than
    // the live mechanism); otherwise wraps into [1, 359] — a computed value
    // of exactly 0/360 is remapped to 1 to avoid colliding with that
    // sentinel, and incidentally keeps it inside WP#255 p1's (0, 360)
    // exclusive range too (MSP_SET_HEAD itself has no such restriction).
    int16_t resolveHeadingDeg(const peer_t *peer, double courseDeg) const;
    // Signed along-track distance (meters) from the follower's current position
    // to `target`, in the leader's track frame — positive means the
    // target is ahead of the follower (follower is lagging its slot).
    double resolveAlongTrackErrorM(const FollowTarget &target, double courseDeg) const;
    // Combines the leader's live ground speed and the along-track correction
    // above into a clamped cm/s setpoint.
    int32_t resolveTargetSpeedCmS(const peer_t *peer, const FollowTarget &target, double courseDeg) const;
    // The pilot's autothrottle arm switch. Unassigned (< 1)
    // resolves true — always armed, the default when no switch is
    // configured. Otherwise armed while the channel's pulse width falls
    // within [autothrottleEnableMinThresholdUs, autothrottleEnableMaxThresholdUs]
    // — a closed range rather than a single switch-high threshold so the
    // same two fields can describe a 2-way, 3-way, or 6-pos switch's specific
    // "armed" detent(s). Read live every cycle, no edge-latch.
    bool autothrottleArmed() const;
    // Derives this cycle's GVAR values from current state and sends whichever
    // of the two configured GVARs changed or are due for their
    // heartbeat resend. conditionCode is the caller-computed
    // 0-3 FollowConditionCode value for conditionFlagsGvarIndex — callers
    // combine the altitude-floor clamp, target-too-far, and RC-freeze
    // conditions before calling this.
    void updateStatusGvars(FollowConditionCode conditionCode);
    // Writes the follower's just-computed commanded waypoint (lat/lon
    // converted to a north/east offset in cm from the follower's own
    // position, plus alt/heading as passed to sendFollowWaypoint()) to
    // GVARs 0-3 (FOLLOW_DEBUG_*_GVAR_INDEX) every loop() cycle, gated on
    // config.debug.
    void updateDebugGvars(int32_t lat_1e7, int32_t lon_1e7, int32_t altCm, int16_t headingDeg);
    // Writes autothrottleEngageGvarIndex every cycle (change+heartbeat
    // gated, like updateStatusGvars()), and targetSpeedGvarIndex
    // only when engaged. engaged already reflects the airframe gate
    // and the RC arm switch — callers don't need to check either
    // themselves.
    void updateAutothrottleGvars(bool engaged, int32_t targetSpeedCmS);
};
