#include "FollowManager.h"
#include "FollowInternal.h"
#include "../GNSS/GNSSManager.h"
#include "main.h"
#include <math.h>
#include <EEPROM.h>

// EEPROM region begins immediately after cfg's own footprint (main.h) —
// see ConfigHandler.cpp's config_init(), which reserves
// sizeof(cfg) + sizeof(FollowEepromRecord) total via EEPROM.begin() so
// this region is always available once EEPROM.begin() has run.
#define FOLLOW_EEPROM_OFFSET sizeof(cfg)

// Minimum time between EEPROM commits — guards against a
// stuck/spammed "Save to EEPROM" button hammering flash with writes.
#define FOLLOW_EEPROM_COMMIT_MIN_INTERVAL_MS 2000

// Below this horizontal offset magnitude, a slot is considered "stacked"
// (overhead/underneath) for the purposes of the minimum-vertical-separation
// rule.
#define FOLLOW_STACKED_HORIZONTAL_EPSILON_M 0.5

// Tolerance for rcCandidateMatchesStaticDefault()'s "RC candidate must
// reproduce the static default" pre-arm rule, below — absorbs RC read
// quantization/jitter (frac steps in 1/500 increments) without weakening
// the check into a sign-only match.
#define FOLLOW_PREARM_MATCH_EPSILON_M 0.05

// How often to resend a GVAR even if its value hasn't changed, so a
// single dropped MSP write doesn't leave the OSD showing a stale state
// indefinitely. 20x less frequent than the default 4 Hz
// waypoint stream — negligible added MSP traffic.
#define FOLLOW_GVAR_HEARTBEAT_MS 5000

FollowTarget slotToLatLon(int32_t peer_lat_1e6, int32_t peer_lon_1e6, double course_deg,
                          double long_m, double lat_m)
{
    // Everything from here to the final lround() is done in double
    // precision and rounded exactly once. peer_lat_1e6/lon are int32 fixed
    // point, course_deg comes from an int16 tenths-of-a-degree field, and
    // long_m/lat_m are configured meters — rounding any of those
    // individually before combining them would throw away precision that
    // the final result can't get back.
    double th = radians(course_deg);
    double north_m = long_m * cos(th) - lat_m * sin(th); // ahead-unit + right-unit
    double east_m  = long_m * sin(th) + lat_m * cos(th);

    double distance_m = sqrt(north_m * north_m + east_m * east_m);
    double bearing_deg = degrees(atan2(east_m, north_m));
    if (bearing_deg < 0.0)
    {
        bearing_deg += 360.0;
    }

    GNSSLocation origin{};
    origin.lat = (double)peer_lat_1e6 / 1e6;
    origin.lon = (double)peer_lon_1e6 / 1e6;

    // Reuse the existing, already-proven great-circle projection rather than
    // hand-rolling a flat-earth approximation.
    GNSSLocation projected = GNSSManager::calculatePointAtDistance(origin, distance_m, bearing_deg);

    FollowTarget target;
    target.lat_1e7 = (int32_t)lround(projected.lat * 1e7);
    target.lon_1e7 = (int32_t)lround(projected.lon * 1e7);
    return target;
}

FollowManager::FollowManager(IFollowMsp *msp, IFollowGnss *gnss, IFollowPeers *peers)
    : msp(msp), gnss(gnss), peers(peers)
{
}

// FollowManager::getSingleton() is defined in FollowProdAdapters.cpp,
// alongside the real adapters it wires FollowManager up to -- kept out of
// this file so the native test env (which excludes FollowProdAdapters.cpp)
// never needs to link the concrete MSPManager/GNSSManager/PeerManager
// singletons.

bool FollowManager::followSwitchActive()
{
    switch (FOLLOW_TRIGGER_MODE)
    {
        case FOLLOW_TRIGGER_GCSNAV:
            return msp->isGCSNavActive();
        case FOLLOW_TRIGGER_AUX:
        default:
            // AUX-channel trigger is not yet implemented — never active
            // rather than silently defaulting on.
            return false;
    }
}

const peer_t *FollowManager::resolveLock()
{
    if (state == FOLLOW_LOCK_IDLE)
    {
        state = FOLLOW_LOCK_ACQUIRING;
    }

    if (state == FOLLOW_LOCK_ACQUIRING)
    {
        const peer_t *candidate = nullptr;

        if (config.targetPeer != 0)
        {
            const peer_t *p = peers->getPeerById(config.targetPeer);
            if (p != nullptr && !peer_is_stale(p, config.peerTimeoutMs))
            {
                candidate = p;
            }
        }
        else
        {
            for (uint8_t i = 0; i < NODES_MAX; i++)
            {
                const peer_t *p = peers->getPeer(i);
                if (p != nullptr && p->id > 0 && !peer_is_stale(p, config.peerTimeoutMs))
                {
                    candidate = p;
                    break;
                }
            }
        }

        if (candidate == nullptr)
        {
            return nullptr; // still acquiring
        }

        lockedId = candidate->id;
        strncpy(lockedName, candidate->name, sizeof(lockedName) - 1);
        lockedName[sizeof(lockedName) - 1] = '\0';
        state = FOLLOW_LOCK_LOCKED;
        return candidate;
    }

    if (state == FOLLOW_LOCK_LOCKED)
    {
        const peer_t *p = peers->getPeerById(lockedId);
        if (p == nullptr || peer_is_stale(p, config.peerTimeoutMs))
        {
            state = FOLLOW_LOCK_LOCKED_HOLDING;
            return nullptr;
        }
        return p;
    }

    // FOLLOW_LOCK_LOCKED_HOLDING: keep checking the same locked id for
    // freshness every cycle, but never scan for or switch to another peer —
    // no automatic failover.
    const peer_t *p = peers->getPeerById(lockedId);
    if (p != nullptr && !peer_is_stale(p, config.peerTimeoutMs))
    {
        if (strncmp(p->name, lockedName, sizeof(lockedName)) == 0)
        {
            state = FOLLOW_LOCK_LOCKED;
            return p;
        }
        // The LoRa slot id was reassigned to a different aircraft while we
        // were holding. Treat as a lost lock: clear the
        // id so this can never resume, rather than silently following a
        // new aircraft under the old id. Only a gate cycle (switch off/on)
        // can recover from here.
        lockedId = 0;
    }
    return nullptr;
}

void FollowManager::forceReacquire()
{
    state = FOLLOW_LOCK_ACQUIRING;
    lockedId = 0;
    lockedName[0] = '\0';
}

double FollowManager::resolveCourseDeg(const peer_t *peer)
{
    // peer->gps.groundSpeed is int16 cm/s; minCourseSpeed is human-facing
    // m/s. Convert at the comparison site — comparing the raw values
    // directly (e.g. "200 < 2") would almost never trip and would silently
    // defeat this fallback.
    int16_t minSpeedCmS = (int16_t)lround(config.minCourseSpeed * 100.0);

    if (peer->gps.groundSpeed >= minSpeedCmS)
    {
        haveValidCourse = true;
        lastValidCourseDeg = (double)peer->gps.groundCourse / 10.0;
        return lastValidCourseDeg;
    }

    // Leader's below-threshold — hold the last known course rather than
    // letting the slot swing on GPS course jitter while stationary.
    if (haveValidCourse)
    {
        return lastValidCourseDeg;
    }
    // No valid course captured yet (e.g. leader has been stationary since
    // before acquire) — fall back to whatever's reported rather than an
    // arbitrary 0.
    return (double)peer->gps.groundCourse / 10.0;
}

int16_t FollowManager::resolveHeadingDeg(const peer_t *peer, double courseDeg) const
{
    double raw;
    switch (config.headingMode)
    {
        case FOLLOW_HEADING_COURSE:
            raw = courseDeg;
            break;
        case FOLLOW_HEADING_POINT_LEADER:
        {
            // peer->gps.lat/lon are int32 x1e6 — same conversion
            // slotToLatLon() uses for the leader's own
            // origin point.
            GNSSLocation leaderLoc{};
            leaderLoc.lat = (double)peer->gps.lat / 1e6;
            leaderLoc.lon = (double)peer->gps.lon / 1e6;
            raw = (double)gnss->courseTo(leaderLoc);
            break;
        }
        case FOLLOW_HEADING_FIXED:
            raw = config.headingDeg;
            break;
        case FOLLOW_HEADING_COURSE_RELATIVE:
            raw = courseDeg + config.headingDeg;
            break;
        case FOLLOW_HEADING_OFF:
        default:
            return 0; // wire sentinel: don't update heading this cycle
    }

    int32_t deg = (int32_t)lround(raw) % 360;
    if (deg < 0)
    {
        deg += 360;
    }
    if (deg == 0)
    {
        // 0 is this function's own "don't update heading" wire sentinel (the
        // default: case above) — a computed heading that legitimately wraps
        // to exactly 0/360 (due north) would collide with it, so nudge to 1°
        // instead. Imperceptible in flight. This also happens to keep the
        // value inside WP#255's p1 range (INAV's setWaypoint() only applies
        // p1 when 0 < p1 < 360, both ends exclusive) — currently moot since
        // p1 is inert for a follower in NAV POSHOLD_3D (see
        // MSPManager::sendFollowWaypoint()'s comment), but harmless to keep
        // valid there too against the day that changes. MSP_SET_HEAD itself
        // has no such range restriction.
        deg = 1;
    }
    return (int16_t)deg;
}

// x1e7 lat/lon (FollowTarget's representation) as a GNSSLocation -- shared
// conversion used anywhere a solved target needs to go through IFollowGnss.
static GNSSLocation toGnssLocation(int32_t lat_1e7, int32_t lon_1e7)
{
    GNSSLocation loc{};
    loc.lat = (double)lat_1e7 / 1e7;
    loc.lon = (double)lon_1e7 / 1e7;
    return loc;
}

// Bearing+distance from the follower's own position to `loc`, decomposed
// into a local north/east tangent-plane offset in meters -- shared by
// resolveAlongTrackErrorM() (needs the along-track component) and
// updateDebugGvars() (needs both components directly).
static void horizontalOffsetM(IFollowGnss *gnss, GNSSLocation loc, double *northM, double *eastM)
{
    double distM = gnss->horizontalDistanceTo(loc);
    double bearingRad = radians((double)gnss->courseTo(loc));
    *northM = distM * cos(bearingRad);
    *eastM = distM * sin(bearingRad);
}

double FollowManager::resolveAlongTrackErrorM(const FollowTarget &target, double courseDeg) const
{
    double north_m, east_m;
    horizontalOffsetM(gnss, toGnssLocation(target.lat_1e7, target.lon_1e7), &north_m, &east_m);
    double th = radians(courseDeg);
    return north_m * cos(th) + east_m * sin(th);
}

int32_t FollowManager::resolveTargetSpeedCmS(const peer_t *peer, const FollowTarget &target, double courseDeg) const
{
    double alongTrackErrorM = resolveAlongTrackErrorM(target, courseDeg);

    // Kinematic braking law: v = sqrt(2*a*d) is the closing speed that lets
    // the follower cover along-track error `d` while decelerating at `a` to
    // land exactly on the leader's speed as d reaches 0 — no overshoot, and
    // it naturally ramps hard when far behind/ahead of the slot and tapers
    // smoothly on final approach, unlike a constant-gain linear term. float
    // (sqrtf, not sqrt/double) because this MCU class has no hardware
    // double-precision FPU, and the extra precision buys nothing here — the
    // result is rounded to whole cm/s and clamped below anyway.
    float errorCm = (float)(alongTrackErrorM * 100.0);
    float aCmS2 = (float)config.speedCorrectionAccelCmS2;
    float correctionCmS = copysignf(sqrtf(2.0f * aCmS2 * fabsf(errorCm)), errorCm);
    double targetSpeedCmS = (double)peer->gps.groundSpeed + (double)correctionCmS;

    double minCmS = config.minTargetSpeedMps * 100.0;
    double maxCmS = config.maxTargetSpeedMps * 100.0;
    targetSpeedCmS = constrain(targetSpeedCmS, minCmS, maxCmS);
    return (int32_t)lround(targetSpeedCmS);
}

// Shared by loop() (runtime, has a live peer) and applyConfig() (server-side
// validation of a candidate config with no peer in scope yet) so both
// always agree on the same two geometry rules. Only the two checks that
// depend purely on the configured offset — not on a live peer/target — live
// here; targetTooFar()'s max-distance-from-self check has no equivalent at
// config-validation time and stays in targetTooFar() below.
bool offsetGeometrySane(const FollowOffset &offset, double minSepM, double minVSepM, String *errMsg)
{
    double horizontalMag = sqrt(offset.longitudinal_m * offset.longitudinal_m +
                                 offset.lateral_m * offset.lateral_m);
    double mag3d = sqrt(horizontalMag * horizontalMag + offset.vertical_m * offset.vertical_m);

    // Minimum 3D separation — forbids the degenerate collision slot.
    if (mag3d < minSepM)
    {
        if (errMsg) *errMsg = "slot magnitude is below minSepM (minimum 3D separation)";
        return false;
    }
    // Minimum vertical gap for stacked (overhead/underneath) slots — absorbs
    // GPS vertical error, not just physical clearance.
    if (horizontalMag < FOLLOW_STACKED_HORIZONTAL_EPSILON_M && fabs(offset.vertical_m) < minVSepM)
    {
        if (errMsg) *errMsg = "stacked slot's vertical offset is below minVSepM";
        return false;
    }
    return true;
}

// Second safety layer for RC-scaled offsets: true if `axis` crossing from
// `referenceAxis`'s sign to `candidateAxis`'s sign is unsafe right now. Only
// a genuine sign flip counts as "crossing" — 0 on either side is the
// boundary itself, not a side, so it never counts as a flip. `coMag` is
// the *smaller* of the other two axes' combined magnitude at the
// reference point and at the candidate point — the conservative choice,
// covering two RC-assigned axes swinging in the same cycle.
bool axisSignLocked(double candidateAxis, double referenceAxis,
                            double candidateOther1, double candidateOther2,
                            double referenceOther1, double referenceOther2,
                            double minSepM)
{
    bool crossed = (candidateAxis > 0 && referenceAxis < 0) ||
                   (candidateAxis < 0 && referenceAxis > 0);
    if (!crossed)
    {
        return false;
    }
    double coMagCandidate = sqrt(candidateOther1 * candidateOther1 + candidateOther2 * candidateOther2);
    double coMagReference = sqrt(referenceOther1 * referenceOther1 + referenceOther2 * referenceOther2);
    double coMag = min(coMagCandidate, coMagReference);
    return coMag < minSepM;
}

// Both safety layers (offsetGeometrySane()'s geometry check, then
// axisSignLocked()'s sign-crossing check), evaluated together so there's
// exactly one pass/fail test. Shared, read-only, between resolveOffset()
// (which adopts `candidate` as the new lastKnownGood on a pass) and the
// pre-arm advisory check (rcPreArmCheckFailed, which never mutates state)
// — both callers must always agree on the same answer for the same inputs.
bool candidateOffsetOk(const FollowOffset &candidate, const FollowOffset &reference,
                               double minSepM, double minVSepM)
{
    if (!offsetGeometrySane(candidate, minSepM, minVSepM, nullptr))
    {
        return false;
    }
    if (axisSignLocked(candidate.longitudinal_m, reference.longitudinal_m,
                        candidate.lateral_m, candidate.vertical_m,
                        reference.lateral_m, reference.vertical_m, minSepM))
    {
        return false;
    }
    if (axisSignLocked(candidate.lateral_m, reference.lateral_m,
                        candidate.longitudinal_m, candidate.vertical_m,
                        reference.longitudinal_m, reference.vertical_m, minSepM))
    {
        return false;
    }
    if (axisSignLocked(candidate.vertical_m, reference.vertical_m,
                        candidate.longitudinal_m, candidate.lateral_m,
                        reference.longitudinal_m, reference.lateral_m, minSepM))
    {
        return false;
    }
    return true;
}

// Pre-arm-only, stricter than Layer 1/Layer 2 above: requires every
// RC-assigned axis's resolved candidate to exactly reproduce the saved
// static default, not merely avoid the collision-risk geometry those
// layers guard against. The pilot's stick must sit at the
// one position (full deflection toward the default's sign, since
// resolveAxisOffset() maps center to 0) that reproduces the configured
// value, or the pre-arm warning stays lit. Axes with no RC channel
// assigned always match trivially, since resolveAxisOffset() returns
// configuredM verbatim for them.
bool rcCandidateMatchesStaticDefault(const FollowOffset &candidate, const FollowRuntimeConfig &config)
{
    if (config.rcLongChannel != -1 &&
        fabs(candidate.longitudinal_m - config.ofsLongM) > FOLLOW_PREARM_MATCH_EPSILON_M)
    {
        return false;
    }
    if (config.rcLatChannel != -1 &&
        fabs(candidate.lateral_m - config.ofsLatM) > FOLLOW_PREARM_MATCH_EPSILON_M)
    {
        return false;
    }
    if (config.rcVertChannel != -1 &&
        fabs(candidate.vertical_m - config.ofsVertM) > FOLLOW_PREARM_MATCH_EPSILON_M)
    {
        return false;
    }
    return true;
}

double FollowManager::resolveAxisOffset(double configuredM, int16_t channel1Based) const
{
    if (channel1Based < 1)
    {
        return configuredM; // no channel assigned
    }

    uint16_t us;
    if (!msp->getRcChannelUs((uint8_t)channel1Based, &us))
    {
        return configuredM; // no FC connected, or channel1Based out of MSP_RC's range
    }
    // Whatever value comes back is mapped as-is, including below 1000us --
    // there is no separate "invalid reading" case once a channel is
    // assigned. The clamp below already guards over/under-
    // travel, so this also covers an unpopulated channel index (reads 0us
    // from the cached msp_rc_t) the same way: it resolves to
    // -gap, not a fallback to configuredM.

    double gap = fabs(configuredM);
    uint16_t usClamped = constrain(us, 1000, 2000);
    double frac = ((double)usClamped - 1500.0) / 500.0; // -1.0 .. +1.0
    return frac * gap;
}

bool FollowManager::anyRcChannelAssigned() const
{
    return config.rcLongChannel != -1 || config.rcLatChannel != -1 || config.rcVertChannel != -1;
}

bool FollowManager::autothrottleArmed() const
{
    if (config.autothrottleEnableRcChannel < 1)
    {
        return true; // unassigned == always armed
    }
    uint16_t us;
    if (!msp->getRcChannelUs((uint8_t)config.autothrottleEnableRcChannel, &us))
    {
        return true; // no FC connected — same fallback resolveAxisOffset() uses
    }
    return us >= config.autothrottleEnableMinThresholdUs && us <= config.autothrottleEnableMaxThresholdUs;
}

FollowOffset FollowManager::resolveCandidateOffset() const
{
    return {
        resolveAxisOffset(config.ofsLongM, config.rcLongChannel),
        resolveAxisOffset(config.ofsLatM, config.rcLatChannel),
        resolveAxisOffset(config.ofsVertM, config.rcVertChannel),
    };
}

FollowOffset FollowManager::resolveOffset()
{
    FollowOffset candidate = resolveCandidateOffset();

    bool ok = candidateOffsetOk(candidate, lastKnownGood, config.minSepM, config.minVSepM);
    rcSlotFrozen = !ok;
    if (ok)
    {
        lastKnownGood = candidate;
    }
    // On failure, lastKnownGood is left exactly as it was — the freeze.
    // When no axis has an RC channel assigned, `candidate` always equals
    // lastKnownGood already (both are the static config, which
    // applyConfig() guarantees is geometry-sane), so ok is always true and
    // this is a no-op.
    return lastKnownGood;
}

bool FollowManager::targetTooFar(const FollowTarget &target) const
{
    double distFromSelf = gnss->horizontalDistanceTo(toGnssLocation(target.lat_1e7, target.lon_1e7));
    return distFromSelf > config.maxTargetDistM;
}

void FollowManager::loop()
{
    if (sys.phase <= MODE_OTA_SYNC)
    {
        return;
    }
    if (millis() < nextRunTime)
    {
        return;
    }
    nextRunTime = millis() + (1000 / config.emitHz);

    // Every early-exit path below reports the same way: no waypoint this
    // cycle, so the autothrottle GVARs go to their disengaged state and the
    // status GVARs report whatever condition code the caller determined.
    auto bail = [&](FollowConditionCode code) {
        updateStatusGvars(code);
        updateAutothrottleGvars(false, 0);
    };

    // Catch the common "RC disagrees with the static default's sign"
    // bootstrap trap while still on the ground, where the pilot can simply
    // move the stick before it matters mid-flight. Runs independent of the
    // follow gate/GCS NAV — the point is to catch this before the pilot
    // ever reaches for the switch, not just while follow is inactive —
    // gated only on arm state so it can't freeze stale if the gate goes
    // active before the pilot arms. Reset to false every cycle the craft
    // isn't disarmed-with-an-axis-assigned, so it never reports stale while armed.
    rcPreArmCheckFailed = false;
    havePreArmCandidateOffset = false;
    if (msp->getState() == 0 && anyRcChannelAssigned())
    {
        // Read-only: deliberately does not touch lastKnownGood — this is a
        // simulation of "what would happen if follow engaged right now,"
        // not a real state transition.
        preArmCandidateOffset = resolveCandidateOffset();
        havePreArmCandidateOffset = true;
        // Two independent reasons to warn: the collision-geometry check
        // (Layer 1/2) can still pass on a sign-flipped axis if the other
        // two axes already clear minSepM on their own, so it alone doesn't
        // guarantee RC agrees with the static default — the exact-match
        // check below closes that gap.
        rcPreArmCheckFailed = !candidateOffsetOk(preArmCandidateOffset, lastKnownGood, config.minSepM, config.minVSepM) ||
                              !rcCandidateMatchesStaticDefault(preArmCandidateOffset, config);
    }

    if (!followSwitchActive())
    {
        state = FOLLOW_LOCK_IDLE;
        lockedId = 0;
        lockedName[0] = '\0';
        haveValidCourse = false;
        bail(rcPreArmCheckFailed ? FOLLOW_CONDITION_RC_INVALID_GAP_SETTINGS : FOLLOW_CONDITION_NONE);
        return;
    }

    const peer_t *peer = resolveLock();
    if (peer == nullptr)
    {
        bail(FOLLOW_CONDITION_NONE);
        return; // ACQUIRING or LOCKED_HOLDING this cycle — nothing to emit
    }

    FollowOffset offset = resolveOffset();
    double courseDeg = resolveCourseDeg(peer);

    FollowTarget target = slotToLatLon(peer->gps.lat, peer->gps.lon, courseDeg,
                                        offset.longitudinal_m, offset.lateral_m);

    // [B] local_altitude_cm() is the follower's baro/GPS-fused home-relative
    // estimate; peer->relalt is a raw-GPS-only delta (leader minus
    // follower). Summed here in double precision and rounded once, since
    // relalt*100 is exact but offset.vertical_m*100 generally isn't (this
    // frame mixing is a known accuracy bound, not a bug; see
    // FOLLOW_MIN_VSEP_M's GPS-error margin).
    double altCmD = (double)msp->local_altitude_cm()
                   + (double)peer->relalt * 100.0
                   + offset.vertical_m * 100.0;
    int32_t altCm = (int32_t)lround(altCmD);

    // Hard floor: never command the follower below a
    // configurable minimum home-relative altitude, regardless of what the
    // sum above produced — e.g. the leader flying low/landing, or a BELOW
    // slot dragging the follower toward the ground. Clamp, don't reject:
    // unlike the geometry-sane/targetTooFar() checks below, this must not suppress the waypoint —
    // the follower should keep tracking laterally and hold at the floor.
    int32_t floorCm = (int32_t)lround(config.minAltM * 100.0);
    bool floorClamped = altCm < floorCm;
    if (floorClamped)
    {
        altCm = floorCm;
    }

    // Attribute the clamp to RC only if the plain configured
    // (non-RC-scaled) vertical offset would NOT also have clamped. When no
    // channel is assigned to vertical, offset.vertical_m == config.ofsVertM
    // by construction, so this always agrees with the "actual" check, with
    // no special-casing needed.
    bool floorAttributableToRc = false;
    if (floorClamped)
    {
        // Reuses altCmD's already-computed local_altitude_cm()+relalt sum,
        // just swapping in the static (non-RC-scaled) vertical offset,
        // instead of re-deriving the whole sum from scratch.
        double altCmStaticD = altCmD + (config.ofsVertM - offset.vertical_m) * 100.0;
        int32_t altCmStatic = (int32_t)lround(altCmStaticD);
        floorAttributableToRc = altCmStatic >= floorCm;
    }

    // Sequential, single-value condition code — raise to whichever
    // active condition ranks highest this cycle rather than overwriting in
    // call order (see this file's FollowConditionCode comment).
    FollowConditionCode conditionCode = FOLLOW_CONDITION_NONE;
    auto raiseCondition = [&conditionCode](FollowConditionCode candidate) {
        if (candidate > conditionCode) conditionCode = candidate;
    };

    if (floorClamped)
    {
        raiseCondition(floorAttributableToRc ? FOLLOW_CONDITION_RC_INVALID_GAP_SETTINGS : FOLLOW_CONDITION_FLOOR_CLAMPED);
    }
    if (rcSlotFrozen)
    {
        raiseCondition(FOLLOW_CONDITION_RC_INVALID_GAP_SETTINGS);
    }

    if (!offsetGeometrySane(offset, config.minSepM, config.minVSepM, nullptr))
    {
        bail(conditionCode);
        return;
    }

    if (targetTooFar(target))
    {
        raiseCondition(FOLLOW_CONDITION_TARGET_TOO_FAR);
        bail(conditionCode);
        return;
    }

    // Nose heading — independent of the position target above. Computed the
    // same way regardless of follower airframe: unlike the speed
    // autothrottle below, INAV's HEADING HOLD / yaw-rate PID path isn't
    // gated to a specific mixer platform type, so no craft-type branch is
    // needed here.
    int16_t headingDeg = resolveHeadingDeg(peer, courseDeg);

    updateDebugGvars(target.lat_1e7, target.lon_1e7, altCm, headingDeg);

    // Speed autothrottle: gated on a fixed-wing (airplane) mixer
    // on the follower FC and the pilot's arm switch. engaged already folds
    // in both, so downstream consumers of updateAutothrottleGvars() don't
    // need to check either themselves.
    bool autothrottleEngaged = msp->getPlatformType() == INAV_PLATFORM_AIRPLANE
                             && autothrottleArmed();
    int32_t targetSpeedCmS = autothrottleEngaged ? resolveTargetSpeedCmS(peer, target, courseDeg) : 0;
    updateAutothrottleGvars(autothrottleEngaged, targetSpeedCmS);
    lastAutothrottleEngaged = autothrottleEngaged;
    lastTargetSpeedCmS = targetSpeedCmS;

    // headingDeg is also passed to sendFollowWaypoint() below (WP#255's p1) —
    // currently inert on INAV 9.x for a follower in NAV POSHOLD_3D, kept as a
    // forward-compatible best-effort write (see that function's comment).
    // sendSetHead() (MSP_SET_HEAD) is the mechanism that actually works today.
    msp->sendFollowWaypoint(target.lat_1e7, target.lon_1e7, altCm, headingDeg);
    // headingDeg == 0 is FOLLOW_HEADING_OFF's wire sentinel (resolveHeadingDeg()
    // never returns 0 for any other mode — see its comment) — skip sending in
    // that case rather than commanding due north. isHeadingHoldActive() gates
    // on INAV's own HEADING HOLD box being active, the precondition for this
    // target to actually reach the yaw-rate PID while in NAV POSHOLD_3D
    // (see MSPManager::sendSetHead()'s comment for why).
    if (headingDeg != 0 && msp->isHeadingHoldActive())
    {
        msp->sendSetHead(headingDeg);
    }
    updateStatusGvars(conditionCode);

    haveLastTarget = true;
    lastTarget = target;
    lastTargetAltCm = altCm;
    lastTargetHeadingDeg = headingDeg;
    lastTargetTime = millis();
    lastLiveOffset = offset; // for statusJson()'s liveOffset
}

static const char *lockStateName(FollowLockState state)
{
    switch (state)
    {
        case FOLLOW_LOCK_IDLE:            return "IDLE";
        case FOLLOW_LOCK_ACQUIRING:       return "ACQUIRING";
        case FOLLOW_LOCK_LOCKED:          return "LOCKED";
        case FOLLOW_LOCK_LOCKED_HOLDING:  return "LOCKED_HOLDING";
        default:                          return "UNKNOWN";
    }
}

void FollowManager::statusJson(JsonDocument *doc)
{
    (*doc)["state"] = lockStateName(state);
    (*doc)["gateActive"] = followSwitchActive();
    (*doc)["lockedId"] = lockedId;
    (*doc)["lockedName"] = lockedName;
    if (haveLastTarget)
    {
        JsonObject target = doc->createNestedObject("lastTarget");
        target["lat"] = lastTarget.lat_1e7;
        target["lon"] = lastTarget.lon_1e7;
        target["altCm"] = lastTargetAltCm;
        target["headingDeg"] = lastTargetHeadingDeg;
        target["ageMs"] = millis() - lastTargetTime;
    }
    if (config.statusGvarIndex >= 0 && lastSentStatusGvarValue != INT32_MIN)
    {
        (*doc)["statusGvarValue"] = lastSentStatusGvarValue;
    }
    if (config.conditionFlagsGvarIndex >= 0 && lastSentConditionFlagsGvarValue != INT32_MIN)
    {
        (*doc)["conditionFlagsGvarValue"] = lastSentConditionFlagsGvarValue;
    }
    (*doc)["platformType"] = (int)msp->getPlatformType();
    (*doc)["autothrottleArmed"] = autothrottleArmed();
    if (haveLastTarget) // reuse the same "we've actually computed a target at least once" gate
    {
        (*doc)["targetSpeedCmS"] = lastTargetSpeedCmS;
        (*doc)["autothrottleEngaged"] = lastAutothrottleEngaged;
    }
    if (haveLastTarget)
    {
        JsonObject live = doc->createNestedObject("liveOffset");
        live["longM"] = lastLiveOffset.longitudinal_m;
        live["latM"] = lastLiveOffset.lateral_m;
        live["vertM"] = lastLiveOffset.vertical_m;
        (*doc)["rcSlotFrozen"] = rcSlotFrozen;
    }
    if (havePreArmCandidateOffset)
    {
        JsonObject preArm = doc->createNestedObject("preArmCandidateOffset");
        preArm["longM"] = preArmCandidateOffset.longitudinal_m;
        preArm["latM"] = preArmCandidateOffset.lateral_m;
        preArm["vertM"] = preArmCandidateOffset.vertical_m;
    }
    (*doc)["rcPreArmCheckFailed"] = rcPreArmCheckFailed;
}

// Status code for statusGvarIndex. IDLE only appears transiently (loop() sets it
// right before the gate-inactive early return) — included for
// completeness, not reachable with a nonzero code.
static int32_t statusGvarValue(FollowLockState state, uint8_t lockedId)
{
    switch (state)
    {
        case FOLLOW_LOCK_ACQUIRING:      return 1;
        case FOLLOW_LOCK_LOCKED:         return 2;
        // lockedId == 0 only happens here via the id-reuse-mismatch path in
        // resolveLock() (a different aircraft claimed the same LoRa slot id
        // while we were holding) — reported as code 4 ("ID LOST") to
        // distinguish it from the ordinary holding case (code 3).
        case FOLLOW_LOCK_LOCKED_HOLDING: return lockedId == 0 ? 4 : 3;
        case FOLLOW_LOCK_IDLE:
        default:                          return 0;
    }
}

// Shared "send only if changed or heartbeat-due" rule, used by
// every change+heartbeat GVAR below (status, condition flags, autothrottle
// engage). gvarIndex < 0 means that slot is disabled -- a no-op, leaving
// *lastSent at its INT32_MIN "never sent" sentinel so a later re-enable
// still sends immediately rather than waiting on a stale heartbeat clock.
static void sendGvarIfDue(IFollowMsp *msp, int16_t gvarIndex, int32_t value,
                           int32_t *lastSent, unsigned long *lastSendMs, unsigned long now)
{
    if (gvarIndex < 0)
    {
        return;
    }
    bool due = *lastSent == INT32_MIN
             || value != *lastSent
             || (now - *lastSendMs) >= FOLLOW_GVAR_HEARTBEAT_MS;
    if (due)
    {
        msp->sendGvar((uint8_t)gvarIndex, value);
        *lastSent = value;
        *lastSendMs = now;
    }
}

void FollowManager::updateStatusGvars(FollowConditionCode conditionCode)
{
    unsigned long now = millis();
    sendGvarIfDue(msp, config.statusGvarIndex, statusGvarValue(state, lockedId),
                  &lastSentStatusGvarValue, &lastStatusGvarSendMs, now);
    // conditionCode is the FollowConditionCode value computed by the caller.
    sendGvarIfDue(msp, config.conditionFlagsGvarIndex, conditionCode,
                  &lastSentConditionFlagsGvarValue, &lastConditionFlagsGvarSendMs, now);
}

void FollowManager::updateAutothrottleGvars(bool engaged, int32_t targetSpeedCmS)
{
    unsigned long now = millis();
    sendGvarIfDue(msp, config.autothrottleEngageGvarIndex, engaged ? 1 : 0,
                  &lastSentAutothrottleEngageValue, &lastAutothrottleEngageSendMs, now);
    if (engaged && config.targetSpeedGvarIndex >= 0)
    {
        msp->sendGvar((uint8_t)config.targetSpeedGvarIndex, targetSpeedCmS);
    }
}

// lat_1e7/lon_1e7 are the follower's commanded waypoint (x1e7 degrees, same
// as FollowTarget) — too many digits for any INAV Custom OSD Element's
// numeric display (widest built-in type clamps to ±99999) and not something
// a pilot could sanity-check by eye anyway, so this converts them to the
// target's offset from the follower's own current position (north/east, cm)
// before sending — small enough to display, and directly meaningful ("target
// is ~15m ahead"). altCm is home-relative cm, headingDeg is whole degrees —
// both sent as-is, same as passed to sendFollowWaypoint().
void FollowManager::updateDebugGvars(int32_t lat_1e7, int32_t lon_1e7, int32_t altCm, int16_t headingDeg)
{
    if (!config.debug)
    {
        return;
    }
    double north_m, east_m;
    horizontalOffsetM(gnss, toGnssLocation(lat_1e7, lon_1e7), &north_m, &east_m);
    int32_t northOffsetCm = (int32_t)lround(north_m * 100.0);
    int32_t eastOffsetCm = (int32_t)lround(east_m * 100.0);

    msp->sendGvar(FOLLOW_DEBUG_NORTH_GVAR_INDEX, northOffsetCm);
    msp->sendGvar(FOLLOW_DEBUG_EAST_GVAR_INDEX, eastOffsetCm);
    msp->sendGvar(FOLLOW_DEBUG_ALT_GVAR_INDEX, altCm);
    msp->sendGvar(FOLLOW_DEBUG_HEADING_GVAR_INDEX, headingDeg);
}

static const char *headingModeName(FollowHeadingMode m)
{
    switch (m)
    {
        case FOLLOW_HEADING_COURSE:          return "COURSE";
        case FOLLOW_HEADING_POINT_LEADER:    return "POINT_LEADER";
        case FOLLOW_HEADING_FIXED:           return "FIXED";
        case FOLLOW_HEADING_COURSE_RELATIVE: return "COURSE_RELATIVE";
        case FOLLOW_HEADING_OFF:
        default:                              return "OFF";
    }
}

static const char *triggerModeName(FollowTriggerMode m)
{
    switch (m)
    {
        case FOLLOW_TRIGGER_AUX: return "AUX";
        case FOLLOW_TRIGGER_GCSNAV:
        default:                  return "GCSNAV";
    }
}

// Single source of truth for which FollowRuntimeConfig fields configJson()/
// toEepromRecord()/fromEepromRecord() carry, so adding a field means
// touching this list once instead of three parallel, easy-to-desync
// hand-written copies. Split in two because the EEPROM conversion needs to
// know each field's direction of travel:
//   DIRECT fields are the same type in FollowRuntimeConfig and
//   FollowEepromRecord -- plain assignment both ways.
//   ROUNDED fields are `double` in FollowRuntimeConfig, narrowed to
//   int16_t in FollowEepromRecord -- lround() one way, exact widening back
//   (see fromEepromRecord()'s comment).
// headingMode is DIRECT for EEPROM purposes too, but configJson() reports it
// as a name string rather than a raw value, so it -- like triggerMode
// (compile-time only) and debug (RAM only, not in FollowEepromRecord at
// all) -- is handled by hand in each function instead of going through
// either list.
#define FOLLOW_CONFIG_DIRECT_FIELDS(X) \
    X(targetPeer) X(emitHz) X(peerTimeoutMs) \
    X(statusGvarIndex) X(conditionFlagsGvarIndex) \
    X(rcLongChannel) X(rcLatChannel) X(rcVertChannel) \
    X(targetSpeedGvarIndex) X(autothrottleEngageGvarIndex) \
    X(autothrottleEnableRcChannel) X(autothrottleEnableMinThresholdUs) \
    X(autothrottleEnableMaxThresholdUs) X(speedCorrectionAccelCmS2)
#define FOLLOW_CONFIG_ROUNDED_FIELDS(X) \
    X(ofsLongM) X(ofsLatM) X(ofsVertM) \
    X(minSepM) X(minVSepM) X(maxTargetDistM) X(minAltM) \
    X(minCourseSpeed) X(headingDeg) \
    X(minTargetSpeedMps) X(maxTargetSpeedMps)

void FollowManager::configJson(JsonDocument *doc) const
{
#define JSON_FIELD(field) (*doc)[#field] = config.field;
    FOLLOW_CONFIG_DIRECT_FIELDS(JSON_FIELD)
    FOLLOW_CONFIG_ROUNDED_FIELDS(JSON_FIELD)
#undef JSON_FIELD

    // Trigger mode is compile-time-only (AUX-channel triggering isn't
    // implemented yet) — report it read-only rather than accepting it via
    // applyConfig().
    (*doc)["triggerMode"] = triggerModeName((FollowTriggerMode)FOLLOW_TRIGGER_MODE);
    (*doc)["headingMode"] = headingModeName(config.headingMode);

    // RAM only (see FollowConfig.h's FOLLOW_DEBUG_ENABLED comment) — not
    // in FollowEepromRecord, so this always reports false again after a
    // reboot regardless of what was last applied.
    (*doc)["debug"] = config.debug;
}

bool FollowManager::applyConfig(const FollowRuntimeConfig &newConfig, String *errMsg)
{
    String localErr;
    if (!errMsg) errMsg = &localErr;

    if (newConfig.emitHz == 0)
    {
        *errMsg = "emitHz must be > 0";
        return false;
    }
    if (newConfig.peerTimeoutMs == 0)
    {
        *errMsg = "peerTimeoutMs must be > 0";
        return false;
    }
    if (newConfig.minSepM < 0 || newConfig.minVSepM < 0 || newConfig.minAltM < 0)
    {
        *errMsg = "minSepM/minVSepM/minAltM must be >= 0";
        return false;
    }
    if (newConfig.maxTargetDistM <= 0)
    {
        *errMsg = "maxTargetDistM must be > 0";
        return false;
    }
    if (newConfig.minCourseSpeed < 0)
    {
        *errMsg = "minCourseSpeed must be >= 0";
        return false;
    }
    if (newConfig.targetPeer > NODES_MAX)
    {
        *errMsg = "targetPeer out of range";
        return false;
    }
    if (newConfig.statusGvarIndex < -1 || newConfig.statusGvarIndex > 7)
    {
        *errMsg = "statusGvarIndex must be -1 (disabled) or 0-7";
        return false;
    }
    if (newConfig.conditionFlagsGvarIndex < -1 || newConfig.conditionFlagsGvarIndex > 7)
    {
        *errMsg = "conditionFlagsGvarIndex must be -1 (disabled) or 0-7";
        return false;
    }
    if (newConfig.rcLongChannel != -1 && (newConfig.rcLongChannel < 1 || newConfig.rcLongChannel > MSP_MAX_SUPPORTED_CHANNELS))
    {
        *errMsg = "rcLongChannel must be -1 (disabled) or 1-16";
        return false;
    }
    if (newConfig.rcLatChannel != -1 && (newConfig.rcLatChannel < 1 || newConfig.rcLatChannel > MSP_MAX_SUPPORTED_CHANNELS))
    {
        *errMsg = "rcLatChannel must be -1 (disabled) or 1-16";
        return false;
    }
    if (newConfig.rcVertChannel != -1 && (newConfig.rcVertChannel < 1 || newConfig.rcVertChannel > MSP_MAX_SUPPORTED_CHANNELS))
    {
        *errMsg = "rcVertChannel must be -1 (disabled) or 1-16";
        return false;
    }
    if (newConfig.targetSpeedGvarIndex < -1 || newConfig.targetSpeedGvarIndex > 7)
    {
        *errMsg = "targetSpeedGvarIndex must be -1 (disabled) or 0-7";
        return false;
    }
    if (newConfig.autothrottleEngageGvarIndex < -1 || newConfig.autothrottleEngageGvarIndex > 7)
    {
        *errMsg = "autothrottleEngageGvarIndex must be -1 (disabled) or 0-7";
        return false;
    }
    if (newConfig.autothrottleEnableRcChannel != -1 &&
        (newConfig.autothrottleEnableRcChannel < 1 || newConfig.autothrottleEnableRcChannel > MSP_MAX_SUPPORTED_CHANNELS))
    {
        *errMsg = "autothrottleEnableRcChannel must be -1 (disabled) or 1-16";
        return false;
    }

    // A raw POST bypassing the web UI could set overlapping GVAR
    // indices/RC channels and the firmware would accept it, so these checks
    // are enforced here as well as in html/follow-logic.js's
    // validateConfig(), giving REST-level and UI-level clients the same
    // guarantee.
    if ((newConfig.statusGvarIndex != -1 && newConfig.statusGvarIndex == newConfig.conditionFlagsGvarIndex) ||
        (newConfig.statusGvarIndex != -1 && newConfig.statusGvarIndex == newConfig.targetSpeedGvarIndex) ||
        (newConfig.statusGvarIndex != -1 && newConfig.statusGvarIndex == newConfig.autothrottleEngageGvarIndex) ||
        (newConfig.conditionFlagsGvarIndex != -1 && newConfig.conditionFlagsGvarIndex == newConfig.targetSpeedGvarIndex) ||
        (newConfig.conditionFlagsGvarIndex != -1 && newConfig.conditionFlagsGvarIndex == newConfig.autothrottleEngageGvarIndex) ||
        (newConfig.targetSpeedGvarIndex != -1 && newConfig.targetSpeedGvarIndex == newConfig.autothrottleEngageGvarIndex))
    {
        *errMsg = "GVAR indices must be unique (or -1/disabled)";
        return false;
    }
    if ((newConfig.rcLongChannel != -1 && newConfig.rcLongChannel == newConfig.rcLatChannel) ||
        (newConfig.rcLongChannel != -1 && newConfig.rcLongChannel == newConfig.rcVertChannel) ||
        (newConfig.rcLatChannel != -1 && newConfig.rcLatChannel == newConfig.rcVertChannel))
    {
        *errMsg = "rcLongChannel/rcLatChannel/rcVertChannel must be unique (or -1/disabled)";
        return false;
    }
    if (newConfig.autothrottleEnableRcChannel != -1 &&
        (newConfig.autothrottleEnableRcChannel == newConfig.rcLongChannel ||
         newConfig.autothrottleEnableRcChannel == newConfig.rcLatChannel ||
         newConfig.autothrottleEnableRcChannel == newConfig.rcVertChannel))
    {
        *errMsg = "autothrottleEnableRcChannel must differ from the RC axis channels (or -1/disabled)";
        return false;
    }
    if (newConfig.autothrottleEnableMaxThresholdUs <= newConfig.autothrottleEnableMinThresholdUs)
    {
        *errMsg = "autothrottleEnableMaxThresholdUs must be > autothrottleEnableMinThresholdUs";
        return false;
    }

    // Only matters once a pilot has actually wired up an arm channel --
    // i.e. intends to use autothrottle. Left gated behind that so the
    // compiled-in 0/0 defaults (an invalid range on their own) can sit
    // there un-configured until the pilot explicitly enters both values
    // for their specific airframe.
    if (newConfig.autothrottleEnableRcChannel != -1 &&
        (newConfig.minTargetSpeedMps <= 0 || newConfig.maxTargetSpeedMps <= newConfig.minTargetSpeedMps))
    {
        *errMsg = "minTargetSpeedMps must be > 0 and maxTargetSpeedMps must be > minTargetSpeedMps when autothrottleEnableRcChannel is set";
        return false;
    }
    if (newConfig.speedCorrectionAccelCmS2 < 0)
    {
        // This is a magnitude fed through copysignf() in
        // resolveTargetSpeedCmS() — a negative value would flip the
        // correction to push the follower the wrong way.
        *errMsg = "speedCorrectionAccelCmS2 must be >= 0";
        return false;
    }

    // Offset geometry rules, evaluated against the config's canonical
    // offset — mirrors loop()'s offsetGeometrySane() call so a config that's
    // accepted here can never be rejected by that same check later.
    FollowOffset offset = { newConfig.ofsLongM, newConfig.ofsLatM, newConfig.ofsVertM };
    if (!offsetGeometrySane(offset, newConfig.minSepM, newConfig.minVSepM, errMsg))
    {
        return false;
    }

    bool targetPeerChanged = (newConfig.targetPeer != config.targetPeer);
    config = newConfig;
    // A config change (new gaps, a reassigned channel) can make the
    // previously-frozen triple meaningless, so re-anchor it to the new
    // static offset — the one point applyConfig() already guarantees is
    // geometry-sane via the offsetGeometrySane() check above.
    lastKnownGood = { config.ofsLongM, config.ofsLatM, config.ofsVertM };
    if (targetPeerChanged)
    {
        forceReacquire();
    }
    return true;
}

// Converts a runtime config to its narrower on-disk form (see
// FollowEepromRecord's comment in FollowManager.h for why int16_t loses no
// precision here). lround(), not a plain cast, so a value the UI couldn't
// have produced anyway (e.g. one seeded from a compile-time #define with a
// fractional value) rounds to the nearest representable integer instead of
// silently truncating toward zero.
static FollowEepromRecord toEepromRecord(const FollowRuntimeConfig &config)
{
    FollowEepromRecord record{};
    record.version = FOLLOW_EEPROM_VERSION;

#define COPY_DIRECT(field) record.field = config.field;
    FOLLOW_CONFIG_DIRECT_FIELDS(COPY_DIRECT)
#undef COPY_DIRECT
#define COPY_ROUNDED(field) record.field = (int16_t)lround(config.field);
    FOLLOW_CONFIG_ROUNDED_FIELDS(COPY_ROUNDED)
#undef COPY_ROUNDED
    record.headingMode = config.headingMode;

    return record;
}

// Converts the on-disk record back to a runtime config. The int16_t->double
// widening here is always exact (every integer int16_t can represent is
// exactly representable as a double), so unlike toEepromRecord() above this
// direction has no rounding to reason about.
static FollowRuntimeConfig fromEepromRecord(const FollowEepromRecord &record)
{
    FollowRuntimeConfig config;

#define COPY_DIRECT(field) config.field = record.field;
    FOLLOW_CONFIG_DIRECT_FIELDS(COPY_DIRECT)
#undef COPY_DIRECT
#define COPY_WIDEN(field) config.field = record.field;
    FOLLOW_CONFIG_ROUNDED_FIELDS(COPY_WIDEN)
#undef COPY_WIDEN
    config.headingMode = record.headingMode;

    return config;
}
#undef FOLLOW_CONFIG_DIRECT_FIELDS
#undef FOLLOW_CONFIG_ROUNDED_FIELDS

void FollowManager::loadFromEEPROM()
{
    FollowEepromRecord record;
    EEPROM.get(FOLLOW_EEPROM_OFFSET, record);
    if (record.version != FOLLOW_EEPROM_VERSION)
    {
        // Fresh flash / nothing saved yet — keep the compile-time defaults
        // FollowRuntimeConfig's member initializers already seeded.
        return;
    }
    // Reuse applyConfig()'s validation (offset geometry rules + basic field sanity) so
    // a corrupted or stale-schema record (bit flips, a struct layout that
    // changed since it was written) can't silently arm follow with insane
    // geometry. forceReacquire() is a no-op here — no peer lock exists yet
    // at boot.
    String errMsg;
    if (!applyConfig(fromEepromRecord(record), &errMsg))
    {
        DBGF("[FollowManager] ignoring invalid EEPROM config: %s\n", errMsg.c_str());
    }
}

bool FollowManager::saveToEEPROM(String *errMsg)
{
    String localErr;
    if (!errMsg) errMsg = &localErr;

    unsigned long now = millis();
    if (lastEepromCommitMs != 0 && now - lastEepromCommitMs < FOLLOW_EEPROM_COMMIT_MIN_INTERVAL_MS)
    {
        *errMsg = "saved too recently, try again shortly";
        return false;
    }

    FollowEepromRecord record = toEepromRecord(config);
    EEPROM.put(FOLLOW_EEPROM_OFFSET, record);
    EEPROM.commit();
    lastEepromCommitMs = now;
    return true;
}
