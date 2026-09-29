#pragma once
//
// Follow: steer this node's iNav flight controller into a formation slot behind
// (or beside/above) another node's live position.
//
// This is the v2 port of the Follow feature contributed to v1 (src/lib/Follow,
// kept there as unbuilt reference). The control law, safety layers, GVAR status
// reporting and config validation are carried over unchanged; what changed is
// the seams, which now match the Node core:
//
//   - peers come from the UID-keyed ff::PeerTable (Node::peers()), not the v1
//     slot-id PeerManager. The target is a 32-bit UID (0 = nearest peer), and
//     because a UID is a stable identity the v1 "slot id reused by a different
//     aircraft" hold-state hack is gone.
//   - our own position comes from ff::ILocationSource, the same source the
//     beacon uses; geodesy is the pure ff::geo module.
//   - the flight controller is behind IFollowFc, implemented by a non-blocking
//     MSP adapter (hal/MspFcLink). No call here ever waits on a serial port.
//   - time is injected (service(now_ms)); no millis(), no globals, so the whole
//     controller runs under host tests with fakes.
//   - persistence is not its problem: the whole node configuration, this block
//     included, is one JSON document owned by ff_core/config.h and written to
//     LittleFS by hal/ConfigStore. v1's per-feature EEPROM record is gone.
//
#include <cstddef>
#include <cstdint>

#include "msp_fc.h"
#include "node.h"

// ---- Compile-time defaults ----------------------------------------------------
// These seed FollowConfig. Every value is #ifndef-guarded so a target's
// build_flags can override it without editing this file. Geometry/timing values
// that get combined with GPS-derived doubles are doubles, not floats, so no
// precision is thrown away in the target math.

enum FollowTriggerMode {
    FOLLOW_TRIGGER_GCSNAV = 0,
    FOLLOW_TRIGGER_AUX = 1,
};

// Commanded nose heading, sent via WP#255's p1 field and MSP_SET_HEAD.
enum FollowHeadingMode {
    FOLLOW_HEADING_OFF = 0,              // don't touch heading
    FOLLOW_HEADING_COURSE = 1,           // leader's direction of travel
    FOLLOW_HEADING_POINT_LEADER = 2,     // bearing toward the leader's live position
    FOLLOW_HEADING_FIXED = 3,            // FOLLOW_HEADING_DEG as an absolute heading
    FOLLOW_HEADING_COURSE_RELATIVE = 4,  // FOLLOW_HEADING_DEG added to the course
};

// Default slot: chase-high (behind, centred, above), as track-relative metres.
#ifndef FOLLOW_OFS_LONG_M
#define FOLLOW_OFS_LONG_M -15.0
#endif
#ifndef FOLLOW_OFS_LAT_M
#define FOLLOW_OFS_LAT_M 0.0
#endif
#ifndef FOLLOW_OFS_VERT_M
#define FOLLOW_OFS_VERT_M 10.0
#endif
// Below this 3D slot magnitude, refuse to arm.
#ifndef FOLLOW_MIN_SEP_M
#define FOLLOW_MIN_SEP_M 8.0
#endif
// Minimum |vertical| when both horizontal components are ~0 (stacked slot).
#ifndef FOLLOW_MIN_VSEP_M
#define FOLLOW_MIN_VSEP_M 13.0
#endif
// Runtime sanity bound on solved target distance from the follower.
#ifndef FOLLOW_MAX_TARGET_DIST_M
#define FOLLOW_MAX_TARGET_DIST_M 50.0
#endif
// Absolute floor on the commanded home-relative altitude (a clamp, not a reject).
#ifndef FOLLOW_MIN_ALT_M
#define FOLLOW_MIN_ALT_M 3.0
#endif
#ifndef FOLLOW_TRIGGER_MODE
#define FOLLOW_TRIGGER_MODE FOLLOW_TRIGGER_GCSNAV
#endif
// 0 = NEAREST (lock onto the closest followable peer at acquire time);
// nonzero = pin acquisition to that specific peer UID.
#ifndef FOLLOW_TARGET_UID
#define FOLLOW_TARGET_UID 0
#endif
#ifndef FOLLOW_EMIT_HZ
#define FOLLOW_EMIT_HZ 4
#endif
#ifndef FOLLOW_PEER_TIMEOUT_MS
#define FOLLOW_PEER_TIMEOUT_MS 1500
#endif
// m/s; below this leader speed the last valid course is held.
#ifndef FOLLOW_MIN_COURSE_SPEED
#define FOLLOW_MIN_COURSE_SPEED 2.0
#endif
#ifndef FOLLOW_HEADING_MODE
#define FOLLOW_HEADING_MODE FOLLOW_HEADING_POINT_LEADER
#endif
#ifndef FOLLOW_HEADING_DEG
#define FOLLOW_HEADING_DEG 0.0
#endif
// GVAR indices, -1 = disabled (zero MSP traffic until a pilot opts in).
#ifndef FOLLOW_STATUS_GVAR_INDEX
#define FOLLOW_STATUS_GVAR_INDEX -1
#endif
#ifndef FOLLOW_CONDITION_FLAGS_GVAR_INDEX
#define FOLLOW_CONDITION_FLAGS_GVAR_INDEX -1
#endif
// RC axis control: 1-based MSP_RC channel per axis, -1 = disabled.
#ifndef FOLLOW_RC_LONG_CHANNEL
#define FOLLOW_RC_LONG_CHANNEL -1
#endif
#ifndef FOLLOW_RC_LAT_CHANNEL
#define FOLLOW_RC_LAT_CHANNEL -1
#endif
#ifndef FOLLOW_RC_VERT_CHANNEL
#define FOLLOW_RC_VERT_CHANNEL -1
#endif
// Debug GVAR output (RAM-only toggle, never persisted).
#ifndef FOLLOW_DEBUG_ENABLED
#define FOLLOW_DEBUG_ENABLED false
#endif
#ifndef FOLLOW_DEBUG_NORTH_GVAR_INDEX
#define FOLLOW_DEBUG_NORTH_GVAR_INDEX 0
#endif
#ifndef FOLLOW_DEBUG_EAST_GVAR_INDEX
#define FOLLOW_DEBUG_EAST_GVAR_INDEX 1
#endif
#ifndef FOLLOW_DEBUG_ALT_GVAR_INDEX
#define FOLLOW_DEBUG_ALT_GVAR_INDEX 2
#endif
#ifndef FOLLOW_DEBUG_HEADING_GVAR_INDEX
#define FOLLOW_DEBUG_HEADING_GVAR_INDEX 3
#endif
// Speed autothrottle (fixed-wing followers).
#ifndef FOLLOW_TARGET_SPEED_GVAR_INDEX
#define FOLLOW_TARGET_SPEED_GVAR_INDEX -1
#endif
#ifndef FOLLOW_AUTOTHROTTLE_ENGAGE_GVAR_INDEX
#define FOLLOW_AUTOTHROTTLE_ENGAGE_GVAR_INDEX -1
#endif
#ifndef FOLLOW_AUTOTHROTTLE_ENABLE_RC_CHANNEL
#define FOLLOW_AUTOTHROTTLE_ENABLE_RC_CHANNEL -1
#endif
#ifndef FOLLOW_AUTOTHROTTLE_ENABLE_MIN_THRESHOLD_US
#define FOLLOW_AUTOTHROTTLE_ENABLE_MIN_THRESHOLD_US 1700
#endif
#ifndef FOLLOW_AUTOTHROTTLE_ENABLE_MAX_THRESHOLD_US
#define FOLLOW_AUTOTHROTTLE_ENABLE_MAX_THRESHOLD_US 2100
#endif
// cm/s^2 for the slot-lag kinematic braking law; 0 = feedforward only.
#ifndef FOLLOW_SPEED_CORRECTION_ACCEL_CMS2
#define FOLLOW_SPEED_CORRECTION_ACCEL_CMS2 0
#endif
// m/s clamp bounds; 0/0 is deliberately invalid until the pilot enters both.
#ifndef FOLLOW_MIN_TARGET_SPEED_MPS
#define FOLLOW_MIN_TARGET_SPEED_MPS 0.0
#endif
#ifndef FOLLOW_MAX_TARGET_SPEED_MPS
#define FOLLOW_MAX_TARGET_SPEED_MPS 0.0
#endif

namespace ff {

// Below this horizontal offset magnitude a slot counts as "stacked".
constexpr double kFollowStackedHorizontalEpsilonM = 0.5;
// Tolerance for the pre-arm "RC candidate reproduces the static default" rule.
constexpr double kFollowPrearmMatchEpsilonM = 0.05;
// Resend an unchanged GVAR at least this often (one dropped write can't leave
// the OSD stale forever).
constexpr uint32_t kFollowGvarHeartbeatMs = 5000;

enum FollowLockState : uint8_t {
    FOLLOW_LOCK_IDLE = 0,
    FOLLOW_LOCK_ACQUIRING = 1,
    FOLLOW_LOCK_LOCKED = 2,
    FOLLOW_LOCK_LOCKED_HOLDING = 3,
};

// Condition-code table reported via conditionFlagsGvarIndex. Sequential, not a
// bitmask: when several are true in a cycle the highest value wins. Never
// renumber (pilots wire INAV logic conditions against these numbers).
enum FollowConditionCode : uint8_t {
    FOLLOW_CONDITION_NONE = 0,
    FOLLOW_CONDITION_FLOOR_CLAMPED = 1,
    FOLLOW_CONDITION_TARGET_TOO_FAR = 2,
    FOLLOW_CONDITION_RC_INVALID_GAP_SETTINGS = 3,
};

// Resolved 3D slot offset in the leader's track-relative frame, metres.
struct FollowOffset {
    double longitudinal_m;  // +ahead / -behind
    double lateral_m;       // +right / -left
    double vertical_m;      // +above / -below
};

struct FollowTarget {
    int32_t lat_1e7;
    int32_t lon_1e7;
};

// Runtime-editable config. Seeded from the compile-time defaults above.
struct FollowConfig {
    double ofsLongM = FOLLOW_OFS_LONG_M;
    double ofsLatM = FOLLOW_OFS_LAT_M;
    double ofsVertM = FOLLOW_OFS_VERT_M;

    uint32_t targetUid = FOLLOW_TARGET_UID;  // 0 = nearest followable peer
    uint16_t emitHz = FOLLOW_EMIT_HZ;
    uint32_t peerTimeoutMs = FOLLOW_PEER_TIMEOUT_MS;

    double minSepM = FOLLOW_MIN_SEP_M;
    double minVSepM = FOLLOW_MIN_VSEP_M;
    double maxTargetDistM = FOLLOW_MAX_TARGET_DIST_M;
    double minAltM = FOLLOW_MIN_ALT_M;

    double minCourseSpeed = FOLLOW_MIN_COURSE_SPEED;

    FollowHeadingMode headingMode = FOLLOW_HEADING_MODE;
    double headingDeg = FOLLOW_HEADING_DEG;

    int16_t statusGvarIndex = FOLLOW_STATUS_GVAR_INDEX;
    int16_t conditionFlagsGvarIndex = FOLLOW_CONDITION_FLAGS_GVAR_INDEX;

    int16_t rcLongChannel = FOLLOW_RC_LONG_CHANNEL;
    int16_t rcLatChannel = FOLLOW_RC_LAT_CHANNEL;
    int16_t rcVertChannel = FOLLOW_RC_VERT_CHANNEL;

    int16_t targetSpeedGvarIndex = FOLLOW_TARGET_SPEED_GVAR_INDEX;
    int16_t autothrottleEngageGvarIndex = FOLLOW_AUTOTHROTTLE_ENGAGE_GVAR_INDEX;
    int16_t autothrottleEnableRcChannel = FOLLOW_AUTOTHROTTLE_ENABLE_RC_CHANNEL;
    int16_t autothrottleEnableMinThresholdUs = FOLLOW_AUTOTHROTTLE_ENABLE_MIN_THRESHOLD_US;
    int16_t autothrottleEnableMaxThresholdUs = FOLLOW_AUTOTHROTTLE_ENABLE_MAX_THRESHOLD_US;
    int16_t speedCorrectionAccelCmS2 = FOLLOW_SPEED_CORRECTION_ACCEL_CMS2;
    double minTargetSpeedMps = FOLLOW_MIN_TARGET_SPEED_MPS;
    double maxTargetSpeedMps = FOLLOW_MAX_TARGET_SPEED_MPS;

    // RAM only: never persisted, always false again after a reboot.
    bool debug = FOLLOW_DEBUG_ENABLED;
};

// Field lists shared by the persistence codec (and, later, a JSON layer), so a
// new field is added in one place. DIRECT fields keep their type in the record;
// ROUNDED fields are doubles narrowed to int16_t metres/degrees/m/s (the web UI
// only ever produced integers for them, and int16 covers every sane value).
#define FOLLOW_CONFIG_DIRECT_FIELDS(X)                                      \
    X(targetUid) X(emitHz) X(peerTimeoutMs) X(statusGvarIndex)              \
    X(conditionFlagsGvarIndex) X(rcLongChannel) X(rcLatChannel)             \
    X(rcVertChannel) X(targetSpeedGvarIndex) X(autothrottleEngageGvarIndex) \
    X(autothrottleEnableRcChannel) X(autothrottleEnableMinThresholdUs)      \
    X(autothrottleEnableMaxThresholdUs) X(speedCorrectionAccelCmS2)
#define FOLLOW_CONFIG_ROUNDED_FIELDS(X)                                  \
    X(ofsLongM) X(ofsLatM) X(ofsVertM) X(minSepM) X(minVSepM)            \
    X(maxTargetDistM) X(minAltM) X(minCourseSpeed) X(headingDeg)         \
    X(minTargetSpeedMps) X(maxTargetSpeedMps)

// The flight controller, as Follow sees it. Reads are answered from a cache the
// adapter keeps fresh on its own schedule; writes are buffered, never awaited.
class IFollowFc {
public:
    virtual ~IFollowFc() = default;
    virtual bool armed() = 0;
    virtual bool gcsNavActive() = 0;
    virtual bool headingHoldActive() = 0;
    // 1-based channel. Returns false only if no FC is connected or the channel
    // is out of MSP_RC's range; a transient poll miss returns the last value.
    virtual bool rcChannelUs(uint8_t channel1Based, uint16_t* out_us) = 0;
    // Home-relative altitude estimate, cm (MSP_ALTITUDE).
    virtual int32_t localAltitudeCm() = 0;
    virtual FcPlatform platformType() = 0;
    // INAV follow-me special waypoint #255. heading_deg 1-360, or 0 = leave it.
    virtual void sendFollowWaypoint(int32_t lat_1e7, int32_t lon_1e7, int32_t alt_cm,
                                    int16_t heading_deg) = 0;
    virtual void sendSetHead(int16_t heading_deg) = 0;
    virtual void sendGvar(uint8_t index, int32_t value) = 0;
    // Hint from the controller about which polled telemetry it currently
    // needs, so the adapter can skip MSP_ALTITUDE / MSP_RC traffic otherwise.
    virtual void setTelemetryNeeds(bool /*altitude*/, bool /*rc*/) {}
};

// Read-only snapshot for status reporting (the future web UI / display).
struct FollowStatus {
    FollowLockState state = FOLLOW_LOCK_IDLE;
    bool gateActive = false;
    uint32_t lockedUid = 0;
    char lockedName[kMaxNameLen + 1] = {0};

    bool haveLastTarget = false;
    FollowTarget lastTarget{};
    int32_t lastTargetAltCm = 0;
    int16_t lastTargetHeadingDeg = 0;
    uint32_t lastTargetAgeMs = 0;

    bool haveStatusGvarValue = false;
    int32_t statusGvarValue = 0;
    bool haveConditionFlagsGvarValue = false;
    int32_t conditionFlagsGvarValue = 0;

    FcPlatform platformType = FcPlatform::Unknown;
    bool autothrottleArmed = false;
    int32_t targetSpeedCmS = 0;    // meaningful when haveLastTarget
    bool autothrottleEngaged = false;

    FollowOffset liveOffset{};     // meaningful when haveLastTarget
    bool rcSlotFrozen = false;
    bool havePreArmCandidateOffset = false;
    FollowOffset preArmCandidateOffset{};
    bool rcPreArmCheckFailed = false;
};

const char* followLockStateName(FollowLockState s);
const char* followHeadingModeName(FollowHeadingMode m);
const char* followTriggerModeName(FollowTriggerMode m);

// ---- Pure helpers (exposed for tests and reuse) ------------------------------

// Projects a leader position + track-relative offset to an absolute lat/lon.
// Positions are deg * 1e7 (the wire/MSP format); course is plain degrees.
FollowTarget slotToLatLon(int32_t peer_lat_1e7, int32_t peer_lon_1e7, double course_deg,
                          double long_m, double lat_m);

// Geometry rules that depend only on the configured offset.
bool offsetGeometrySane(const FollowOffset& offset, double minSepM, double minVSepM,
                        const char** err);
bool axisSignLocked(double candidateAxis, double referenceAxis, double candidateOther1,
                    double candidateOther2, double referenceOther1, double referenceOther2,
                    double minSepM);
bool candidateOffsetOk(const FollowOffset& candidate, const FollowOffset& reference,
                       double minSepM, double minVSepM);
bool rcCandidateMatchesStaticDefault(const FollowOffset& candidate, const FollowConfig& config);

// Every rule applyConfig() enforces, as a free function so the config layer can
// validate a candidate Follow block without needing a controller instance.
// *err is left pointing at a static message on failure.
bool followValidateConfig(const FollowConfig& cfg, const char** err = nullptr);

// A peer is stale if absent/invalid, beaconing without a GPS fix, or its last
// *position* is older than timeout_ms (an announce alone does not make a peer
// followable).
bool followPeerStale(const Peer* peer, uint32_t now_ms, uint32_t timeout_ms);

// ---- The controller ----------------------------------------------------------

class FollowController {
public:
    // All three are borrowed, process-lifetime pointers.
    FollowController(const PeerTable* peers, ILocationSource* self, IFollowFc* fc);

    // Call every loop iteration; runs one control cycle when emitHz says so.
    void service(uint32_t now_ms);

    const FollowConfig& config() const { return config_; }
    // Validates newConfig; on success swaps it in atomically (forcing a fresh
    // peer acquire if targetUid changed) and returns true. On failure leaves the
    // live config untouched and points *err at a static message.
    bool applyConfig(const FollowConfig& newConfig, const char** err = nullptr);

    FollowStatus status(uint32_t now_ms) const;

private:
    bool followSwitchActive() const;
    const Peer* resolveLock(uint32_t now_ms);
    void forceReacquire();
    FollowOffset resolveOffset();
    double resolveAxisOffset(double configuredM, int16_t channel1Based) const;
    FollowOffset resolveCandidateOffset() const;
    bool anyRcChannelAssigned() const;
    double resolveCourseDeg(const Peer* peer);
    bool targetTooFar(const FollowTarget& target, const NodeLocation& self) const;
    int16_t resolveHeadingDeg(const Peer* peer, double courseDeg, const NodeLocation& self) const;
    double resolveAlongTrackErrorM(const FollowTarget& target, double courseDeg,
                                   const NodeLocation& self) const;
    int32_t resolveTargetSpeedCmS(const Peer* peer, const FollowTarget& target, double courseDeg,
                                  const NodeLocation& self) const;
    bool autothrottleArmed() const;
    void updateStatusGvars(FollowConditionCode conditionCode, uint32_t now_ms);
    void updateDebugGvars(int32_t lat_1e7, int32_t lon_1e7, int32_t altCm, int16_t headingDeg,
                          const NodeLocation& self);
    void updateAutothrottleGvars(bool engaged, int32_t targetSpeedCmS, uint32_t now_ms);

    const PeerTable* peers_;
    ILocationSource* self_;
    IFollowFc* fc_;

    FollowConfig config_;
    bool started_ = false;
    uint32_t nextRunMs_ = 0;

    FollowLockState state_ = FOLLOW_LOCK_IDLE;
    uint32_t lockedUid_ = 0;
    char lockedName_[kMaxNameLen + 1] = {0};
    double lastValidCourseDeg_ = 0.0;
    bool haveValidCourse_ = false;

    bool haveLastTarget_ = false;
    FollowTarget lastTarget_{};
    int32_t lastTargetAltCm_ = 0;
    int16_t lastTargetHeadingDeg_ = 0;
    uint32_t lastTargetMs_ = 0;

    // INT32_MIN = "never sent" so the very first cycle always sends.
    int32_t lastSentStatusGvarValue_;
    int32_t lastSentConditionFlagsGvarValue_;
    uint32_t lastStatusGvarSendMs_ = 0;
    uint32_t lastConditionFlagsGvarSendMs_ = 0;
    int32_t lastSentAutothrottleEngageValue_;
    uint32_t lastAutothrottleEngageSendMs_ = 0;

    int32_t lastTargetSpeedCmS_ = 0;
    bool lastAutothrottleEngaged_ = false;

    FollowOffset lastKnownGood_{FOLLOW_OFS_LONG_M, FOLLOW_OFS_LAT_M, FOLLOW_OFS_VERT_M};
    bool rcSlotFrozen_ = false;
    bool rcPreArmCheckFailed_ = false;
    FollowOffset preArmCandidateOffset_{};
    bool havePreArmCandidateOffset_ = false;
    FollowOffset lastLiveOffset_{};
};

}  // namespace ff
