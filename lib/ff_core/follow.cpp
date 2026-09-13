#include "follow.h"

#include <climits>
#include <cmath>
#include <cstring>

#include "geo.h"

namespace ff {

namespace {

// v1's Arduino constrain() semantics: with lo > hi the result is lo for values
// below lo and hi otherwise -- notably (0, 0) clamps everything to 0. std::clamp
// would be UB there, and the autothrottle relies on that case (unconfigured
// speed bounds resolve to a 0 setpoint).
double clampD(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

double deg1e7(int32_t v) { return static_cast<double>(v) / 1e7; }

// Bearing+distance from self to (lat, lon), decomposed into a local north/east
// tangent-plane offset in metres.
void horizontalOffsetM(const NodeLocation& self, double lat, double lon, double* northM,
                       double* eastM) {
    const double selfLat = deg1e7(self.lat);
    const double selfLon = deg1e7(self.lon);
    const double distM = geo::distanceM(selfLat, selfLon, lat, lon);
    const double bearingRad = geo::toRad(geo::bearingDeg(selfLat, selfLon, lat, lon));
    *northM = distM * std::cos(bearingRad);
    *eastM = distM * std::sin(bearingRad);
}

// Status code for statusGvarIndex. IDLE only appears transiently (service()
// sets it right before the gate-inactive early return).
int32_t statusGvarValue(FollowLockState state) {
    switch (state) {
        case FOLLOW_LOCK_ACQUIRING:
            return 1;
        case FOLLOW_LOCK_LOCKED:
            return 2;
        case FOLLOW_LOCK_LOCKED_HOLDING:
            return 3;
        case FOLLOW_LOCK_IDLE:
        default:
            return 0;
    }
}

// Shared "send only if changed or heartbeat-due" rule for every
// change+heartbeat GVAR (status, condition flags, autothrottle engage).
// gvarIndex < 0 means that slot is disabled -- a no-op, leaving *lastSent at its
// INT32_MIN "never sent" sentinel so a later re-enable still sends immediately.
void sendGvarIfDue(IFollowFc* fc, int16_t gvarIndex, int32_t value, int32_t* lastSent,
                   uint32_t* lastSendMs, uint32_t now) {
    if (gvarIndex < 0) {
        return;
    }
    const bool due = *lastSent == INT32_MIN || value != *lastSent ||
                     (now - *lastSendMs) >= kFollowGvarHeartbeatMs;
    if (due) {
        fc->sendGvar(static_cast<uint8_t>(gvarIndex), value);
        *lastSent = value;
        *lastSendMs = now;
    }
}

}  // namespace

// ---- Names ------------------------------------------------------------------

const char* followLockStateName(FollowLockState s) {
    switch (s) {
        case FOLLOW_LOCK_IDLE:
            return "IDLE";
        case FOLLOW_LOCK_ACQUIRING:
            return "ACQUIRING";
        case FOLLOW_LOCK_LOCKED:
            return "LOCKED";
        case FOLLOW_LOCK_LOCKED_HOLDING:
            return "LOCKED_HOLDING";
        default:
            return "UNKNOWN";
    }
}

const char* followHeadingModeName(FollowHeadingMode m) {
    switch (m) {
        case FOLLOW_HEADING_COURSE:
            return "COURSE";
        case FOLLOW_HEADING_POINT_LEADER:
            return "POINT_LEADER";
        case FOLLOW_HEADING_FIXED:
            return "FIXED";
        case FOLLOW_HEADING_COURSE_RELATIVE:
            return "COURSE_RELATIVE";
        case FOLLOW_HEADING_OFF:
        default:
            return "OFF";
    }
}

const char* followTriggerModeName(FollowTriggerMode m) {
    switch (m) {
        case FOLLOW_TRIGGER_AUX:
            return "AUX";
        case FOLLOW_TRIGGER_GCSNAV:
        default:
            return "GCSNAV";
    }
}

// ---- Pure helpers -------------------------------------------------------------

FollowTarget slotToLatLon(int32_t peer_lat_1e7, int32_t peer_lon_1e7, double course_deg,
                          double long_m, double lat_m) {
    // Everything up to the final lround() is double precision, rounded once.
    const double th = geo::toRad(course_deg);
    const double north_m = long_m * std::cos(th) - lat_m * std::sin(th);  // ahead + right
    const double east_m = long_m * std::sin(th) + lat_m * std::cos(th);

    const double distance_m = std::sqrt(north_m * north_m + east_m * east_m);
    double bearing_deg = geo::toDeg(std::atan2(east_m, north_m));
    if (bearing_deg < 0.0) {
        bearing_deg += 360.0;
    }

    double lat = 0.0;
    double lon = 0.0;
    geo::pointAtDistance(deg1e7(peer_lat_1e7), deg1e7(peer_lon_1e7), distance_m, bearing_deg, lat,
                         lon);

    FollowTarget target;
    target.lat_1e7 = static_cast<int32_t>(std::lround(lat * 1e7));
    target.lon_1e7 = static_cast<int32_t>(std::lround(lon * 1e7));
    return target;
}

bool offsetGeometrySane(const FollowOffset& offset, double minSepM, double minVSepM,
                        const char** err) {
    const double horizontalMag =
        std::sqrt(offset.longitudinal_m * offset.longitudinal_m + offset.lateral_m * offset.lateral_m);
    const double mag3d = std::sqrt(horizontalMag * horizontalMag + offset.vertical_m * offset.vertical_m);

    // Minimum 3D separation -- forbids the degenerate collision slot.
    if (mag3d < minSepM) {
        if (err) *err = "slot magnitude is below minSepM (minimum 3D separation)";
        return false;
    }
    // Minimum vertical gap for stacked (overhead/underneath) slots -- absorbs
    // GPS vertical error, not just physical clearance.
    if (horizontalMag < kFollowStackedHorizontalEpsilonM && std::fabs(offset.vertical_m) < minVSepM) {
        if (err) *err = "stacked slot's vertical offset is below minVSepM";
        return false;
    }
    return true;
}

// Second safety layer for RC-scaled offsets: true if `axis` crossing from
// referenceAxis's sign to candidateAxis's sign is unsafe right now. Only a
// genuine sign flip counts -- 0 on either side is the boundary, not a side.
// coMag is the *smaller* of the other two axes' combined magnitude at the
// reference and candidate points (covers two axes swinging in one cycle).
bool axisSignLocked(double candidateAxis, double referenceAxis, double candidateOther1,
                    double candidateOther2, double referenceOther1, double referenceOther2,
                    double minSepM) {
    const bool crossed = (candidateAxis > 0 && referenceAxis < 0) ||
                         (candidateAxis < 0 && referenceAxis > 0);
    if (!crossed) {
        return false;
    }
    const double coMagCandidate =
        std::sqrt(candidateOther1 * candidateOther1 + candidateOther2 * candidateOther2);
    const double coMagReference =
        std::sqrt(referenceOther1 * referenceOther1 + referenceOther2 * referenceOther2);
    const double coMag = coMagCandidate < coMagReference ? coMagCandidate : coMagReference;
    return coMag < minSepM;
}

bool candidateOffsetOk(const FollowOffset& candidate, const FollowOffset& reference,
                       double minSepM, double minVSepM) {
    if (!offsetGeometrySane(candidate, minSepM, minVSepM, nullptr)) {
        return false;
    }
    if (axisSignLocked(candidate.longitudinal_m, reference.longitudinal_m, candidate.lateral_m,
                       candidate.vertical_m, reference.lateral_m, reference.vertical_m, minSepM)) {
        return false;
    }
    if (axisSignLocked(candidate.lateral_m, reference.lateral_m, candidate.longitudinal_m,
                       candidate.vertical_m, reference.longitudinal_m, reference.vertical_m,
                       minSepM)) {
        return false;
    }
    if (axisSignLocked(candidate.vertical_m, reference.vertical_m, candidate.longitudinal_m,
                       candidate.lateral_m, reference.longitudinal_m, reference.lateral_m,
                       minSepM)) {
        return false;
    }
    return true;
}

// Pre-arm-only, stricter than the two layers above: every RC-assigned axis's
// resolved candidate must reproduce the saved static default (stick at the one
// position that does), or the pre-arm warning stays lit. Unassigned axes match
// trivially since resolveAxisOffset() returns configuredM verbatim for them.
bool rcCandidateMatchesStaticDefault(const FollowOffset& candidate, const FollowConfig& config) {
    if (config.rcLongChannel != -1 &&
        std::fabs(candidate.longitudinal_m - config.ofsLongM) > kFollowPrearmMatchEpsilonM) {
        return false;
    }
    if (config.rcLatChannel != -1 &&
        std::fabs(candidate.lateral_m - config.ofsLatM) > kFollowPrearmMatchEpsilonM) {
        return false;
    }
    if (config.rcVertChannel != -1 &&
        std::fabs(candidate.vertical_m - config.ofsVertM) > kFollowPrearmMatchEpsilonM) {
        return false;
    }
    return true;
}

bool followPeerStale(const Peer* peer, uint32_t now_ms, uint32_t timeout_ms) {
    if (peer == nullptr || !peer->valid) {
        return true;
    }
    // A beacon sent without a fix carries no usable position (the leader is
    // still on the bench, or lost GPS); never chase it.
    if ((peer->flags & POSITION_FLAG_HAS_FIX) == 0) {
        return true;
    }
    return (now_ms - peer->last_position_ms) > timeout_ms;
}

// ---- Controller ---------------------------------------------------------------

FollowController::FollowController(const PeerTable* peers, ILocationSource* self, IFollowFc* fc)
    : peers_(peers),
      self_(self),
      fc_(fc),
      lastSentStatusGvarValue_(INT32_MIN),
      lastSentConditionFlagsGvarValue_(INT32_MIN),
      lastSentAutothrottleEngageValue_(INT32_MIN) {}

bool FollowController::followSwitchActive() const {
    switch (FOLLOW_TRIGGER_MODE) {
        case FOLLOW_TRIGGER_GCSNAV:
            return fc_->gcsNavActive();
        case FOLLOW_TRIGGER_AUX:
        default:
            // AUX-channel trigger is not implemented -- never active rather
            // than silently defaulting on.
            return false;
    }
}

const Peer* FollowController::resolveLock(uint32_t now_ms) {
    if (state_ == FOLLOW_LOCK_IDLE) {
        state_ = FOLLOW_LOCK_ACQUIRING;
    }

    if (state_ == FOLLOW_LOCK_ACQUIRING) {
        const Peer* candidate = nullptr;
        if (config_.targetUid != 0) {
            const Peer* p = peers_->find(config_.targetUid);
            if (!followPeerStale(p, now_ms, config_.peerTimeoutMs)) {
                candidate = p;
            }
        } else {
            // Nearest followable peer, not merely the first in the table: slot
            // order is allocation order with LRU eviction, so "first" is
            // arbitrary. With several aircraft up, the closest one is the one
            // the pilot means. Needs our own fix to measure from; without one,
            // fall back to table order (any lock is better than none, and the
            // waypoint path is suppressed until we have a fix anyway).
            const NodeLocation self = self_->getLocation();
            double bestDistM = 0.0;
            for (size_t i = 0; i < peers_->capacity(); i++) {
                const Peer* p = peers_->at(i);
                if (followPeerStale(p, now_ms, config_.peerTimeoutMs)) {
                    continue;
                }
                if (!self.valid) {
                    candidate = p;
                    break;
                }
                const double distM = geo::distanceM(deg1e7(self.lat), deg1e7(self.lon),
                                                    deg1e7(p->lat), deg1e7(p->lon));
                if (candidate == nullptr || distM < bestDistM) {
                    candidate = p;
                    bestDistM = distM;
                }
            }
        }
        if (candidate == nullptr) {
            return nullptr;  // still acquiring
        }
        lockedUid_ = candidate->uid;
        std::strncpy(lockedName_, candidate->name, sizeof(lockedName_) - 1);
        lockedName_[sizeof(lockedName_) - 1] = '\0';
        state_ = FOLLOW_LOCK_LOCKED;
        return candidate;
    }

    if (state_ == FOLLOW_LOCK_LOCKED) {
        const Peer* p = peers_->find(lockedUid_);
        if (followPeerStale(p, now_ms, config_.peerTimeoutMs)) {
            state_ = FOLLOW_LOCK_LOCKED_HOLDING;
            return nullptr;
        }
        return p;
    }

    // FOLLOW_LOCK_LOCKED_HOLDING: keep checking the same UID for freshness
    // every cycle, but never scan for or switch to another peer -- no automatic
    // failover. A UID is a stable identity, so if it comes back it is the same
    // aircraft (v1 had to compare names to guard against slot-id reuse).
    const Peer* p = peers_->find(lockedUid_);
    if (!followPeerStale(p, now_ms, config_.peerTimeoutMs)) {
        std::strncpy(lockedName_, p->name, sizeof(lockedName_) - 1);
        lockedName_[sizeof(lockedName_) - 1] = '\0';
        state_ = FOLLOW_LOCK_LOCKED;
        return p;
    }
    return nullptr;
}

void FollowController::forceReacquire() {
    state_ = FOLLOW_LOCK_ACQUIRING;
    lockedUid_ = 0;
    lockedName_[0] = '\0';
}

double FollowController::resolveCourseDeg(const Peer* peer) {
    // peer->speed_cms is cm/s; minCourseSpeed is human-facing m/s. Convert at
    // the comparison site.
    const int32_t minSpeedCmS = static_cast<int32_t>(std::lround(config_.minCourseSpeed * 100.0));

    if (static_cast<int32_t>(peer->speed_cms) >= minSpeedCmS) {
        haveValidCourse_ = true;
        lastValidCourseDeg_ = static_cast<double>(peer->course_ddeg) / 10.0;
        return lastValidCourseDeg_;
    }
    // Leader's below threshold -- hold the last known course rather than
    // letting the slot swing on GPS course jitter while stationary.
    if (haveValidCourse_) {
        return lastValidCourseDeg_;
    }
    // No valid course captured yet -- fall back to whatever's reported.
    return static_cast<double>(peer->course_ddeg) / 10.0;
}

int16_t FollowController::resolveHeadingDeg(const Peer* peer, double courseDeg,
                                            const NodeLocation& self) const {
    double raw;
    switch (config_.headingMode) {
        case FOLLOW_HEADING_COURSE:
            raw = courseDeg;
            break;
        case FOLLOW_HEADING_POINT_LEADER:
            raw = geo::bearingDeg(deg1e7(self.lat), deg1e7(self.lon), deg1e7(peer->lat),
                                  deg1e7(peer->lon));
            break;
        case FOLLOW_HEADING_FIXED:
            raw = config_.headingDeg;
            break;
        case FOLLOW_HEADING_COURSE_RELATIVE:
            raw = courseDeg + config_.headingDeg;
            break;
        case FOLLOW_HEADING_OFF:
        default:
            return 0;  // wire sentinel: don't update heading this cycle
    }

    int32_t deg = static_cast<int32_t>(std::lround(raw)) % 360;
    if (deg < 0) {
        deg += 360;
    }
    if (deg == 0) {
        // 0 is the "don't update heading" sentinel; a genuine due-north heading
        // is nudged to 1 degree (imperceptible), which also keeps it inside
        // WP#255 p1's exclusive (0, 360) range.
        deg = 1;
    }
    return static_cast<int16_t>(deg);
}

double FollowController::resolveAlongTrackErrorM(const FollowTarget& target, double courseDeg,
                                                 const NodeLocation& self) const {
    double north_m;
    double east_m;
    horizontalOffsetM(self, deg1e7(target.lat_1e7), deg1e7(target.lon_1e7), &north_m, &east_m);
    const double th = geo::toRad(courseDeg);
    return north_m * std::cos(th) + east_m * std::sin(th);
}

int32_t FollowController::resolveTargetSpeedCmS(const Peer* peer, const FollowTarget& target,
                                                double courseDeg, const NodeLocation& self) const {
    const double alongTrackErrorM = resolveAlongTrackErrorM(target, courseDeg, self);

    // Kinematic braking law: v = sqrt(2*a*d) is the closing speed that covers
    // along-track error d while decelerating at a to land exactly on the
    // leader's speed as d reaches 0 -- no overshoot. float on purpose: no
    // hardware double FPU on this MCU class and the result is rounded anyway.
    const float errorCm = static_cast<float>(alongTrackErrorM * 100.0);
    const float aCmS2 = static_cast<float>(config_.speedCorrectionAccelCmS2);
    const float correctionCmS = copysignf(sqrtf(2.0f * aCmS2 * fabsf(errorCm)), errorCm);
    double targetSpeedCmS = static_cast<double>(peer->speed_cms) + static_cast<double>(correctionCmS);

    const double minCmS = config_.minTargetSpeedMps * 100.0;
    const double maxCmS = config_.maxTargetSpeedMps * 100.0;
    targetSpeedCmS = clampD(targetSpeedCmS, minCmS, maxCmS);
    return static_cast<int32_t>(std::lround(targetSpeedCmS));
}

double FollowController::resolveAxisOffset(double configuredM, int16_t channel1Based) const {
    if (channel1Based < 1) {
        return configuredM;  // no channel assigned
    }
    uint16_t us;
    if (!fc_->rcChannelUs(static_cast<uint8_t>(channel1Based), &us)) {
        return configuredM;  // no FC connected, or channel out of MSP_RC's range
    }
    // Whatever comes back is mapped as-is, including below 1000us -- the clamp
    // covers over/under-travel and an unpopulated channel (0us) alike.
    const double gap = std::fabs(configuredM);
    const uint16_t usClamped = us < 1000 ? 1000 : (us > 2000 ? 2000 : us);
    const double frac = (static_cast<double>(usClamped) - 1500.0) / 500.0;  // -1..+1
    return frac * gap;
}

bool FollowController::anyRcChannelAssigned() const {
    return config_.rcLongChannel != -1 || config_.rcLatChannel != -1 || config_.rcVertChannel != -1;
}

bool FollowController::autothrottleArmed() const {
    if (config_.autothrottleEnableRcChannel < 1) {
        return true;  // unassigned == always armed
    }
    uint16_t us;
    if (!fc_->rcChannelUs(static_cast<uint8_t>(config_.autothrottleEnableRcChannel), &us)) {
        return true;  // no FC connected -- same fallback resolveAxisOffset() uses
    }
    return us >= config_.autothrottleEnableMinThresholdUs &&
           us <= config_.autothrottleEnableMaxThresholdUs;
}

FollowOffset FollowController::resolveCandidateOffset() const {
    return {
        resolveAxisOffset(config_.ofsLongM, config_.rcLongChannel),
        resolveAxisOffset(config_.ofsLatM, config_.rcLatChannel),
        resolveAxisOffset(config_.ofsVertM, config_.rcVertChannel),
    };
}

FollowOffset FollowController::resolveOffset() {
    const FollowOffset candidate = resolveCandidateOffset();
    const bool ok = candidateOffsetOk(candidate, lastKnownGood_, config_.minSepM, config_.minVSepM);
    rcSlotFrozen_ = !ok;
    if (ok) {
        lastKnownGood_ = candidate;
    }
    // On failure lastKnownGood_ is left exactly as it was -- the freeze. With
    // no RC axis assigned, candidate == lastKnownGood_ (both the static config,
    // which applyConfig() guarantees is geometry-sane), so this is a no-op.
    return lastKnownGood_;
}

bool FollowController::targetTooFar(const FollowTarget& target, const NodeLocation& self) const {
    const double distFromSelf = geo::distanceM(deg1e7(self.lat), deg1e7(self.lon),
                                               deg1e7(target.lat_1e7), deg1e7(target.lon_1e7));
    return distFromSelf > config_.maxTargetDistM;
}

void FollowController::service(uint32_t now_ms) {
    if (started_ && static_cast<int32_t>(now_ms - nextRunMs_) < 0) {
        return;
    }
    started_ = true;
    nextRunMs_ = now_ms + (1000u / config_.emitHz);

    // Tell the FC adapter what polled telemetry this cycle's config needs, so an
    // idle Follow costs no MSP_ALTITUDE / MSP_RC traffic.
    const bool gateActive = followSwitchActive();
    fc_->setTelemetryNeeds(gateActive,
                           anyRcChannelAssigned() || config_.autothrottleEnableRcChannel >= 1);

    // Every early-exit path below reports the same way: no waypoint this cycle,
    // autothrottle GVARs to their disengaged state, status GVARs to the
    // condition code the caller determined.
    auto bail = [&](FollowConditionCode code) {
        updateStatusGvars(code, now_ms);
        updateAutothrottleGvars(false, 0, now_ms);
    };

    // Catch the "RC disagrees with the static default's sign" bootstrap trap
    // while still on the ground. Runs independent of the follow gate, gated only
    // on arm state; reset every cycle so it never reports stale while armed.
    rcPreArmCheckFailed_ = false;
    havePreArmCandidateOffset_ = false;
    if (!fc_->armed() && anyRcChannelAssigned()) {
        // Read-only: does not touch lastKnownGood_.
        preArmCandidateOffset_ = resolveCandidateOffset();
        havePreArmCandidateOffset_ = true;
        rcPreArmCheckFailed_ =
            !candidateOffsetOk(preArmCandidateOffset_, lastKnownGood_, config_.minSepM,
                               config_.minVSepM) ||
            !rcCandidateMatchesStaticDefault(preArmCandidateOffset_, config_);
    }

    if (!gateActive) {
        state_ = FOLLOW_LOCK_IDLE;
        lockedUid_ = 0;
        lockedName_[0] = '\0';
        haveValidCourse_ = false;
        bail(rcPreArmCheckFailed_ ? FOLLOW_CONDITION_RC_INVALID_GAP_SETTINGS
                                  : FOLLOW_CONDITION_NONE);
        return;
    }

    const Peer* peer = resolveLock(now_ms);
    if (peer == nullptr) {
        bail(FOLLOW_CONDITION_NONE);
        return;  // ACQUIRING or LOCKED_HOLDING this cycle -- nothing to emit
    }

    // Our own fix. Without one the distance sanity check and along-track error
    // are meaningless, so no waypoint goes out (the lock is kept).
    const NodeLocation self = self_->getLocation();
    if (!self.valid) {
        bail(FOLLOW_CONDITION_NONE);
        return;
    }

    const FollowOffset offset = resolveOffset();
    const double courseDeg = resolveCourseDeg(peer);

    const FollowTarget target =
        slotToLatLon(peer->lat, peer->lon, courseDeg, offset.longitudinal_m, offset.lateral_m);

    // localAltitudeCm() is the follower's baro/GPS-fused home-relative estimate;
    // (peer.alt_m - self.alt_m) is a raw GPS-only MSL delta. Summed in double and
    // rounded once. This frame mixing is a known accuracy bound (see
    // FOLLOW_MIN_VSEP_M's GPS-error margin), not a bug.
    const double relaltM = static_cast<double>(peer->alt_m) - static_cast<double>(self.alt_m);
    const double altCmD = static_cast<double>(fc_->localAltitudeCm()) + relaltM * 100.0 +
                          offset.vertical_m * 100.0;
    int32_t altCm = static_cast<int32_t>(std::lround(altCmD));

    // Hard floor: clamp, don't reject -- the follower keeps tracking laterally
    // and holds at the floor.
    const int32_t floorCm = static_cast<int32_t>(std::lround(config_.minAltM * 100.0));
    const bool floorClamped = altCm < floorCm;
    if (floorClamped) {
        altCm = floorCm;
    }

    // Attribute the clamp to RC only if the plain configured vertical offset
    // would NOT also have clamped.
    bool floorAttributableToRc = false;
    if (floorClamped) {
        const double altCmStaticD = altCmD + (config_.ofsVertM - offset.vertical_m) * 100.0;
        const int32_t altCmStatic = static_cast<int32_t>(std::lround(altCmStaticD));
        floorAttributableToRc = altCmStatic >= floorCm;
    }

    // Sequential, single-value condition code -- the highest active wins.
    FollowConditionCode conditionCode = FOLLOW_CONDITION_NONE;
    auto raiseCondition = [&conditionCode](FollowConditionCode candidate) {
        if (candidate > conditionCode) conditionCode = candidate;
    };
    if (floorClamped) {
        raiseCondition(floorAttributableToRc ? FOLLOW_CONDITION_RC_INVALID_GAP_SETTINGS
                                             : FOLLOW_CONDITION_FLOOR_CLAMPED);
    }
    if (rcSlotFrozen_) {
        raiseCondition(FOLLOW_CONDITION_RC_INVALID_GAP_SETTINGS);
    }

    if (!offsetGeometrySane(offset, config_.minSepM, config_.minVSepM, nullptr)) {
        bail(conditionCode);
        return;
    }
    if (targetTooFar(target, self)) {
        raiseCondition(FOLLOW_CONDITION_TARGET_TOO_FAR);
        bail(conditionCode);
        return;
    }

    // Nose heading -- independent of the position target. Not airframe-gated:
    // INAV's HEADING HOLD path isn't tied to a mixer platform type.
    const int16_t headingDeg = resolveHeadingDeg(peer, courseDeg, self);

    updateDebugGvars(target.lat_1e7, target.lon_1e7, altCm, headingDeg, self);

    // Speed autothrottle: gated on a fixed-wing mixer and the pilot's arm switch.
    const bool autothrottleEngaged =
        fc_->platformType() == FcPlatform::Airplane && autothrottleArmed();
    const int32_t targetSpeedCmS =
        autothrottleEngaged ? resolveTargetSpeedCmS(peer, target, courseDeg, self) : 0;
    updateAutothrottleGvars(autothrottleEngaged, targetSpeedCmS, now_ms);
    lastAutothrottleEngaged_ = autothrottleEngaged;
    lastTargetSpeedCmS_ = targetSpeedCmS;

    // WP#255's p1 heading is currently inert for a follower in NAV POSHOLD_3D on
    // INAV 9.x (kept as a forward-compatible write); MSP_SET_HEAD is what works
    // today, and only reaches the yaw PID while INAV's HEADING HOLD box is on.
    fc_->sendFollowWaypoint(target.lat_1e7, target.lon_1e7, altCm, headingDeg);
    if (headingDeg != 0 && fc_->headingHoldActive()) {
        fc_->sendSetHead(headingDeg);
    }
    updateStatusGvars(conditionCode, now_ms);

    haveLastTarget_ = true;
    lastTarget_ = target;
    lastTargetAltCm_ = altCm;
    lastTargetHeadingDeg_ = headingDeg;
    lastTargetMs_ = now_ms;
    lastLiveOffset_ = offset;
}

void FollowController::updateStatusGvars(FollowConditionCode conditionCode, uint32_t now_ms) {
    sendGvarIfDue(fc_, config_.statusGvarIndex, statusGvarValue(state_), &lastSentStatusGvarValue_,
                  &lastStatusGvarSendMs_, now_ms);
    sendGvarIfDue(fc_, config_.conditionFlagsGvarIndex, conditionCode,
                  &lastSentConditionFlagsGvarValue_, &lastConditionFlagsGvarSendMs_, now_ms);
}

void FollowController::updateAutothrottleGvars(bool engaged, int32_t targetSpeedCmS,
                                               uint32_t now_ms) {
    sendGvarIfDue(fc_, config_.autothrottleEngageGvarIndex, engaged ? 1 : 0,
                  &lastSentAutothrottleEngageValue_, &lastAutothrottleEngageSendMs_, now_ms);
    if (engaged && config_.targetSpeedGvarIndex >= 0) {
        fc_->sendGvar(static_cast<uint8_t>(config_.targetSpeedGvarIndex), targetSpeedCmS);
    }
}

// Debug GVARs carry the commanded waypoint as a north/east offset from our own
// position in cm (absolute lat/lon has too many digits for an OSD element),
// plus alt and heading as sent.
void FollowController::updateDebugGvars(int32_t lat_1e7, int32_t lon_1e7, int32_t altCm,
                                        int16_t headingDeg, const NodeLocation& self) {
    if (!config_.debug) {
        return;
    }
    double north_m;
    double east_m;
    horizontalOffsetM(self, deg1e7(lat_1e7), deg1e7(lon_1e7), &north_m, &east_m);
    fc_->sendGvar(FOLLOW_DEBUG_NORTH_GVAR_INDEX, static_cast<int32_t>(std::lround(north_m * 100.0)));
    fc_->sendGvar(FOLLOW_DEBUG_EAST_GVAR_INDEX, static_cast<int32_t>(std::lround(east_m * 100.0)));
    fc_->sendGvar(FOLLOW_DEBUG_ALT_GVAR_INDEX, altCm);
    fc_->sendGvar(FOLLOW_DEBUG_HEADING_GVAR_INDEX, headingDeg);
}

FollowStatus FollowController::status(uint32_t now_ms) const {
    FollowStatus s;
    s.state = state_;
    s.gateActive = followSwitchActive();
    s.lockedUid = lockedUid_;
    std::strncpy(s.lockedName, lockedName_, sizeof(s.lockedName) - 1);
    s.lockedName[sizeof(s.lockedName) - 1] = '\0';

    s.haveLastTarget = haveLastTarget_;
    if (haveLastTarget_) {
        s.lastTarget = lastTarget_;
        s.lastTargetAltCm = lastTargetAltCm_;
        s.lastTargetHeadingDeg = lastTargetHeadingDeg_;
        s.lastTargetAgeMs = now_ms - lastTargetMs_;
        s.targetSpeedCmS = lastTargetSpeedCmS_;
        s.autothrottleEngaged = lastAutothrottleEngaged_;
        s.liveOffset = lastLiveOffset_;
        s.rcSlotFrozen = rcSlotFrozen_;
    }
    s.haveStatusGvarValue = config_.statusGvarIndex >= 0 && lastSentStatusGvarValue_ != INT32_MIN;
    s.statusGvarValue = s.haveStatusGvarValue ? lastSentStatusGvarValue_ : 0;
    s.haveConditionFlagsGvarValue =
        config_.conditionFlagsGvarIndex >= 0 && lastSentConditionFlagsGvarValue_ != INT32_MIN;
    s.conditionFlagsGvarValue = s.haveConditionFlagsGvarValue ? lastSentConditionFlagsGvarValue_ : 0;

    s.platformType = fc_->platformType();
    s.autothrottleArmed = autothrottleArmed();

    s.havePreArmCandidateOffset = havePreArmCandidateOffset_;
    if (havePreArmCandidateOffset_) {
        s.preArmCandidateOffset = preArmCandidateOffset_;
    }
    s.rcPreArmCheckFailed = rcPreArmCheckFailed_;
    return s;
}

bool followValidateConfig(const FollowConfig& newConfig, const char** err) {
    const char* localErr = nullptr;
    if (!err) err = &localErr;

    if (newConfig.emitHz == 0) {
        *err = "emitHz must be > 0";
        return false;
    }
    if (newConfig.peerTimeoutMs == 0) {
        *err = "peerTimeoutMs must be > 0";
        return false;
    }
    if (newConfig.minSepM < 0 || newConfig.minVSepM < 0 || newConfig.minAltM < 0) {
        *err = "minSepM/minVSepM/minAltM must be >= 0";
        return false;
    }
    if (newConfig.maxTargetDistM <= 0) {
        *err = "maxTargetDistM must be > 0";
        return false;
    }
    if (newConfig.minCourseSpeed < 0) {
        *err = "minCourseSpeed must be >= 0";
        return false;
    }
    // targetUid: any value is a valid UID (0 = first active); no range rule.
    if (newConfig.statusGvarIndex < -1 || newConfig.statusGvarIndex > 7) {
        *err = "statusGvarIndex must be -1 (disabled) or 0-7";
        return false;
    }
    if (newConfig.conditionFlagsGvarIndex < -1 || newConfig.conditionFlagsGvarIndex > 7) {
        *err = "conditionFlagsGvarIndex must be -1 (disabled) or 0-7";
        return false;
    }
    const int16_t maxCh = static_cast<int16_t>(kMspMaxRcChannels);
    if (newConfig.rcLongChannel != -1 && (newConfig.rcLongChannel < 1 || newConfig.rcLongChannel > maxCh)) {
        *err = "rcLongChannel must be -1 (disabled) or 1-16";
        return false;
    }
    if (newConfig.rcLatChannel != -1 && (newConfig.rcLatChannel < 1 || newConfig.rcLatChannel > maxCh)) {
        *err = "rcLatChannel must be -1 (disabled) or 1-16";
        return false;
    }
    if (newConfig.rcVertChannel != -1 && (newConfig.rcVertChannel < 1 || newConfig.rcVertChannel > maxCh)) {
        *err = "rcVertChannel must be -1 (disabled) or 1-16";
        return false;
    }
    if (newConfig.targetSpeedGvarIndex < -1 || newConfig.targetSpeedGvarIndex > 7) {
        *err = "targetSpeedGvarIndex must be -1 (disabled) or 0-7";
        return false;
    }
    if (newConfig.autothrottleEngageGvarIndex < -1 || newConfig.autothrottleEngageGvarIndex > 7) {
        *err = "autothrottleEngageGvarIndex must be -1 (disabled) or 0-7";
        return false;
    }
    if (newConfig.autothrottleEnableRcChannel != -1 &&
        (newConfig.autothrottleEnableRcChannel < 1 || newConfig.autothrottleEnableRcChannel > maxCh)) {
        *err = "autothrottleEnableRcChannel must be -1 (disabled) or 1-16";
        return false;
    }

    // Overlap rules: enforced here as well as in the web validators so a raw
    // REST client gets the same guarantee.
    if ((newConfig.statusGvarIndex != -1 && newConfig.statusGvarIndex == newConfig.conditionFlagsGvarIndex) ||
        (newConfig.statusGvarIndex != -1 && newConfig.statusGvarIndex == newConfig.targetSpeedGvarIndex) ||
        (newConfig.statusGvarIndex != -1 && newConfig.statusGvarIndex == newConfig.autothrottleEngageGvarIndex) ||
        (newConfig.conditionFlagsGvarIndex != -1 && newConfig.conditionFlagsGvarIndex == newConfig.targetSpeedGvarIndex) ||
        (newConfig.conditionFlagsGvarIndex != -1 && newConfig.conditionFlagsGvarIndex == newConfig.autothrottleEngageGvarIndex) ||
        (newConfig.targetSpeedGvarIndex != -1 && newConfig.targetSpeedGvarIndex == newConfig.autothrottleEngageGvarIndex)) {
        *err = "GVAR indices must be unique (or -1/disabled)";
        return false;
    }
    if ((newConfig.rcLongChannel != -1 && newConfig.rcLongChannel == newConfig.rcLatChannel) ||
        (newConfig.rcLongChannel != -1 && newConfig.rcLongChannel == newConfig.rcVertChannel) ||
        (newConfig.rcLatChannel != -1 && newConfig.rcLatChannel == newConfig.rcVertChannel)) {
        *err = "rcLongChannel/rcLatChannel/rcVertChannel must be unique (or -1/disabled)";
        return false;
    }
    if (newConfig.autothrottleEnableRcChannel != -1 &&
        (newConfig.autothrottleEnableRcChannel == newConfig.rcLongChannel ||
         newConfig.autothrottleEnableRcChannel == newConfig.rcLatChannel ||
         newConfig.autothrottleEnableRcChannel == newConfig.rcVertChannel)) {
        *err = "autothrottleEnableRcChannel must differ from the RC axis channels (or -1/disabled)";
        return false;
    }
    if (newConfig.autothrottleEnableMaxThresholdUs <= newConfig.autothrottleEnableMinThresholdUs) {
        *err = "autothrottleEnableMaxThresholdUs must be > autothrottleEnableMinThresholdUs";
        return false;
    }
    // Only matters once a pilot has wired up an arm channel, so the compiled-in
    // 0/0 defaults can sit un-configured until then.
    if (newConfig.autothrottleEnableRcChannel != -1 &&
        (newConfig.minTargetSpeedMps <= 0 || newConfig.maxTargetSpeedMps <= newConfig.minTargetSpeedMps)) {
        *err = "minTargetSpeedMps must be > 0 and maxTargetSpeedMps must be > minTargetSpeedMps when autothrottleEnableRcChannel is set";
        return false;
    }
    if (newConfig.speedCorrectionAccelCmS2 < 0) {
        // A magnitude fed through copysignf(); negative would push the wrong way.
        *err = "speedCorrectionAccelCmS2 must be >= 0";
        return false;
    }

    // Offset geometry rules against the canonical offset -- mirrors service()'s
    // check so an accepted config can never be rejected by it later.
    const FollowOffset offset = {newConfig.ofsLongM, newConfig.ofsLatM, newConfig.ofsVertM};
    if (!offsetGeometrySane(offset, newConfig.minSepM, newConfig.minVSepM, err)) {
        return false;
    }

    return true;
}

bool FollowController::applyConfig(const FollowConfig& newConfig, const char** err) {
    if (!followValidateConfig(newConfig, err)) {
        return false;
    }

    const bool targetChanged = (newConfig.targetUid != config_.targetUid);
    config_ = newConfig;
    // A config change can make the previously-frozen triple meaningless, so
    // re-anchor it to the new static offset (just proven geometry-sane).
    lastKnownGood_ = {config_.ofsLongM, config_.ofsLatM, config_.ofsVertM};
    if (targetChanged) {
        forceReacquire();
    }
    return true;
}

}  // namespace ff
