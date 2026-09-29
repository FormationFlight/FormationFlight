#pragma once

// Compile-time default configuration for FollowManager. These values seed
// FollowRuntimeConfig (see FollowManager.h), which is itself runtime-editable
// and EEPROM-persisted; every value here is #ifndef-guarded so a target's
// build_flags can override the default without editing this file.
//
// All geometry/timing values that get combined with GPS-derived doubles
// (lat/lon in degrees, altitude in cm) are declared here as `double`, not
// `float`, so no precision is thrown away converting into/out of those
// calculations — see FollowManager.cpp for where these get combined.

enum FollowTriggerMode {
    FOLLOW_TRIGGER_GCSNAV = 0,
    FOLLOW_TRIGGER_AUX = 1,
};

// Commanded nose heading, sent via WP#255's p1 field and (when supported)
// MSP_SET_HEAD — not a second position update. Applies uniformly to
// rotorcraft and fixed-wing followers: unlike the speed autothrottle below,
// INAV's HEADING HOLD / yaw-rate PID path isn't gated to a specific mixer
// platform type, so no craft-type branch is needed here.
enum FollowHeadingMode {
    FOLLOW_HEADING_OFF = 0,             // don't touch heading (p1 = 0, no MSP_SET_HEAD sent)
    FOLLOW_HEADING_COURSE = 1,          // direction of travel (leader's course)
    FOLLOW_HEADING_POINT_LEADER = 2,    // bearing toward the leader's live position
    FOLLOW_HEADING_FIXED = 3,           // FOLLOW_HEADING_DEG as an absolute compass heading
    FOLLOW_HEADING_COURSE_RELATIVE = 4, // FOLLOW_HEADING_DEG added as an offset from course
};

// Named-preset default: chase-high (behind, centered, above) expressed
// directly as canonical track-relative offsets. Testable with the leader
// stationary on the ground, unlike a purely horizontal preset.
// The AHEAD/BEHIND/LEFT/RIGHT/ABOVE/BELOW grid is a UI-only view over these
// signed meters now (see html/follow.js) — FollowManager only ever sees the
// canonical offset.
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
// 5m physical/collision clearance + 8m worst-case GPS vertical error budget.
#ifndef FOLLOW_MIN_VSEP_M
#define FOLLOW_MIN_VSEP_M 13.0
#endif
// Runtime sanity bound on solved target distance from the follower.
#ifndef FOLLOW_MAX_TARGET_DIST_M
#define FOLLOW_MAX_TARGET_DIST_M 50.0
#endif

// Absolute floor on the final commanded home-relative altitude, meters.
// Distinct from FOLLOW_MIN_VSEP_M above: that governs the
// configured slot's separation from the leader; this clamps alt_cm itself
// so a low/descending/landing leader (or a BELOW slot) can never command
// the follower at or below this altitude. Applied as a clamp, not a
// rejection — converted to centimeters at the comparison site.
#ifndef FOLLOW_MIN_ALT_M
#define FOLLOW_MIN_ALT_M 3.0
#endif

#ifndef FOLLOW_TRIGGER_MODE
#define FOLLOW_TRIGGER_MODE FOLLOW_TRIGGER_GCSNAV
#endif

// 0 = FIRST_ACTIVE (lock onto the first non-lost peer with id > 0 at
// acquire time); nonzero = pin acquisition to that specific peer id.
#ifndef FOLLOW_TARGET_PEER
#define FOLLOW_TARGET_PEER 0
#endif

#ifndef FOLLOW_EMIT_HZ
#define FOLLOW_EMIT_HZ 4
#endif

#ifndef FOLLOW_PEER_TIMEOUT_MS
#define FOLLOW_PEER_TIMEOUT_MS 1500
#endif

// m/s, human-facing — converted to cm/s at the comparison site against
// peer->gps.groundSpeed, which is in cm/s.
#ifndef FOLLOW_MIN_COURSE_SPEED
#define FOLLOW_MIN_COURSE_SPEED 2.0
#endif

#ifndef FOLLOW_HEADING_MODE
#define FOLLOW_HEADING_MODE FOLLOW_HEADING_POINT_LEADER
#endif

// Degrees. Meaning depends on FOLLOW_HEADING_MODE: absolute compass heading
// for FIXED, offset added to the leader's live course for COURSE_RELATIVE —
// unused by the other modes.
#ifndef FOLLOW_HEADING_DEG
#define FOLLOW_HEADING_DEG 0.0
#endif

// GVAR index for the primary lock-state indicator, or -1 to
// disable (default — zero MSP traffic, zero OSD dependency until a pilot
// opts in via the web UI). INAV supports indices 0-7.
#ifndef FOLLOW_STATUS_GVAR_INDEX
#define FOLLOW_STATUS_GVAR_INDEX -1
#endif

// GVAR index for the secondary condition-code indicator, or -1
// to disable. Independent of FOLLOW_STATUS_GVAR_INDEX — a pilot can enable
// either, both, or neither. Deliberately generic: only the altitude-floor
// clamp (code 1) is implemented today, but the field/GVAR isn't scoped to
// that one condition — future non-exclusive conditions get code 2, 3, etc.
// on this same slot without another rename.
#ifndef FOLLOW_CONDITION_FLAGS_GVAR_INDEX
#define FOLLOW_CONDITION_FLAGS_GVAR_INDEX -1
#endif

// RC axis control: 1-based MSP_RC channel number per axis, or -1 to disable (default — zero
// MSP_RC polling until a pilot opts in via the web UI, same -1-disables-
// by-default pattern as the GVAR index fields above).
#ifndef FOLLOW_RC_LONG_CHANNEL
#define FOLLOW_RC_LONG_CHANNEL -1
#endif
#ifndef FOLLOW_RC_LAT_CHANNEL
#define FOLLOW_RC_LAT_CHANNEL -1
#endif
#ifndef FOLLOW_RC_VERT_CHANNEL
#define FOLLOW_RC_VERT_CHANNEL -1
#endif

// Debug GVAR output: RAM-only toggle (FollowRuntimeConfig::debug in
// FollowManager.h — deliberately absent from FollowEepromRecord, so it's
// always off again after a reboot, never persisted). When on, the commanded
// target's position relative to the follower's own current location (not
// absolute lat/lon — those are ~9-10 digit numbers that don't fit any INAV
// Custom OSD Element's numeric display, which tops out at 5 digits/±99999)
// plus alt/heading are written to
// these four fixed GVAR indices every loop() cycle, for bench-testing in the
// goggles without dedicating a status/condition GVAR slot to it. Off by
// default.
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

// Speed autothrottle. GVAR indices, -1 = disabled, same convention as the status/condition GVARs
// above. INAV supports indices 0-7.
#ifndef FOLLOW_TARGET_SPEED_GVAR_INDEX
#define FOLLOW_TARGET_SPEED_GVAR_INDEX -1
#endif
#ifndef FOLLOW_AUTOTHROTTLE_ENGAGE_GVAR_INDEX
#define FOLLOW_AUTOTHROTTLE_ENGAGE_GVAR_INDEX -1
#endif
// Pilot's autothrottle arm switch, 1-based MSP_RC channel, or -1
// = unassigned (always armed).
#ifndef FOLLOW_AUTOTHROTTLE_ENABLE_RC_CHANNEL
#define FOLLOW_AUTOTHROTTLE_ENABLE_RC_CHANNEL -1
#endif
// Armed-range bounds (µs), a closed range rather than a single switch-high
// threshold so the same two fields can express a 2-way, 3-way, or 6-pos
// switch's specific "armed" detent(s).
#ifndef FOLLOW_AUTOTHROTTLE_ENABLE_MIN_THRESHOLD_US
#define FOLLOW_AUTOTHROTTLE_ENABLE_MIN_THRESHOLD_US 1700
#endif
#ifndef FOLLOW_AUTOTHROTTLE_ENABLE_MAX_THRESHOLD_US
#define FOLLOW_AUTOTHROTTLE_ENABLE_MAX_THRESHOLD_US 2100
#endif
// cm/s^2. Max closing acceleration/deceleration for the slot-lag kinematic
// braking law (FollowManager::resolveTargetSpeedCmS()). 0 = feedforward-only
// (pure leader-speed mirror) until bench-tuned.
#ifndef FOLLOW_SPEED_CORRECTION_ACCEL_CMS2
#define FOLLOW_SPEED_CORRECTION_ACCEL_CMS2 0
#endif
// m/s. This clamp floor is the feature's only stall-safety mechanism this
// iteration — set it with real margin above the airframe's
// actual stall speed (roughly a third above stall is a reasonable starting
// point), since there is no dynamic sink-rate protection yet. Defaults to 0
// (an invalid range on its own) rather than a number tuned for someone
// else's airframe: applyConfig() requires the pilot to explicitly enter both
// values before the arm channel can be assigned, so autothrottle can never
// engage on an un-reviewed default.
#ifndef FOLLOW_MIN_TARGET_SPEED_MPS
#define FOLLOW_MIN_TARGET_SPEED_MPS 0.0
#endif
#ifndef FOLLOW_MAX_TARGET_SPEED_MPS
#define FOLLOW_MAX_TARGET_SPEED_MPS 0.0
#endif
