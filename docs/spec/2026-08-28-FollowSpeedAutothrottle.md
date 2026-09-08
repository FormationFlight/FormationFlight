# FormationFlight — Follow-Mode Speed Autothrottle (INAV GVAR) — Engineering Spec

**Status:** Draft — not yet planned or implemented
**Target firmware:** FormationFlight (ESP32/ESP8266, PlatformIO/C++)
**Depends on:**
- [`2026-07-31-FollowMeOnInav.md`](2026-07-31-FollowMeOnInav.md) — the `PeerLock` state machine (§6.3) and the position-waypoint stream this feature runs alongside.
- [`2026-08-13-FollowStatusOsdGvar.md`](2026-08-13-FollowStatusOsdGvar.md) — establishes the `MSP2_INAV_SET_GVAR` wire contract (§2), the pilot-assigned-GVAR-index config pattern (§3.4), and the "write an explicit sentinel, don't go silent" convention this spec reuses.
- [`docs/explainers/inav-airspeed-autothrottle.md`](../explainers/inav-airspeed-autothrottle.md) — the ground-speed-hold autothrottle INAV Programming Framework script this spec's GVARs are designed to drive. Not part of FormationFlight; decoded/annotated reference material only.

This is an addendum to both: it adds a third and fourth FF-owned GVAR (on top of the two `2026-08-13` already defines) and reuses the existing `PeerLock`/target-solving machinery from `2026-07-31`, but touches no code from either beyond adding new config fields and two new call sites in `FollowManager::loop()`.

---

## 1. Purpose & Scope

### 1.1 Problem
Follow mode today only commands the follower's **position** (a 3D waypoint, `FollowManager.cpp`'s `slotToLatLon()` + `sendFollowWaypoint()`) and, optionally, its **nose heading**. It has no opinion on the follower's **speed**. In practice this means a fixed-wing follower relies entirely on INAV's own navigation-speed control to close whatever gap exists between its commanded waypoint and its actual position — which is often too sluggish to hold a tight formation slot when the leader accelerates, decelerates, or turns.

The referenced INAV Programming Framework script (`docs/explainers/inav-airspeed-autothrottle.md`) already provides a working ground-speed-hold autothrottle: a Programmable PID drives the aircraft's actual ground speed to a target ground-speed **setpoint**, and directly overrides the throttle channel. Today that setpoint is computed **inside INAV** from a fixed baseline plus two local corrections (a sink-rate nudge, and a pilot's manual RC-channel trim) — it has no notion of a leader or a formation slot at all, because INAV has no such concept.

### 1.2 Goal
Give FF ownership of that setpoint **completely** — not just the feedforward/correction math, but every bound applied to it, so there is exactly one place (`FollowRuntimeConfig`) a pilot ever configures "how fast should this plane go," not one place in FF's web UI and a second, easy-to-forget-about place buried in an INAV Logic Condition literal. Every Follow loop cycle, while a leader is actively locked and the pilot has autothrottle armed via a dedicated RC switch (§3.2), FF computes a target ground speed that:
1. **Feeds forward the leader's own ground speed** (`peer->gps.groundSpeed`, already received via the existing peer telemetry — no new radio payload needed), so the follower's baseline pace matches the leader's, not a fixed compile-time constant.
2. **Corrects for slot lag/lead**: if the follower is behind where its commanded slot position currently is (along the leader's track), nudge the setpoint up; if it's ahead of its slot, nudge it down.
3. **Never leaves the configured envelope**: the sum of the above is clamped to `minTargetSpeedMps`/`maxTargetSpeedMps` (§3.5) as the last step, always. There is deliberately no dynamic sink-rate/stall protection in this iteration (§1.4) — the mitigation is a configuration discipline instead of a reactive boost: `minTargetSpeedMps` is documented, and flagged in the UI, as the floor a pilot should set comfortably above the airframe's actual stall speed (a margin of roughly a third above stall is a reasonable starting point), so the existing clamp absorbs what a sink-rate correction would otherwise have reacted to.

FF writes that number to a pilot-assigned INAV GVAR every cycle, and separately writes a second GVAR that tells INAV's Programming Framework *whether* autothrottle should be engaged at all right now. That flag is FF's AND of three independent, live conditions — an actively-tracked leader, a fixed-wing airframe (§2.4), and the pilot's own RC arm switch (§3.2) — so a pilot can run auto-follow (position-following) with FF-computed autothrottle switched off entirely, just by flipping one RC channel, without touching any web UI config. Losing any one of the three (lost/never-acquired leader, wrong airframe, switch off) falls through to INAV's own regular POSHOLD_3D navigation-speed behavior rather than holding a stale or meaningless setpoint. On the INAV side, PID3 reads this GVAR **directly** as its setpoint (§6) — there is no INAV-side baseline, clamp, or correction of any kind left to duplicate or drift out of sync with FF's config.

This entire mechanism is fixed-wing-specific: the reference script's lever is a **throttle override**, and throttle only maps onto forward ground speed on an airframe where lift and propulsion are decoupled (fixed-wing; also true in principle of a rover/boat, out of scope this iteration — see §1.3). A multirotor has no such lever — its horizontal speed is a function of pitch/roll lean angle under INAV's own nav-speed logic, and it already gets slot-speed-matching for free from that logic reacting to FF's existing position-waypoint stream. So this feature must know the connected FC's airframe type and refuse to engage — in firmware, not just in the UI — on anything other than a fixed-wing platform (§2.4, §3.2, §3.6).

### 1.3 In scope
- Two new pilot-assigned GVAR outputs (§3), following the `2026-08-13` spec's opt-in-via--1, dropdown-in-`html/follow.js`, EEPROM-persisted pattern exactly:
  - `targetSpeedGvarIndex` — the ground-speed setpoint (cm/s), replacing the reference script's `LC20`-`LC38` (baseline + sink-rate correction + pilot trim).
  - `autothrottleEngageGvarIndex` — a 0/1 engage flag, replacing the reference script's `LC27` (`rc.channel[11] > 1480`).
- **A pilot-assigned RC arm switch**, `autothrottleEnableRcChannel` (§3.2, §3.5) — read by FF itself, not by an INAV Logic Condition, the same way `rcLongChannel`/`rcLatChannel`/`rcVertChannel` are already read for trim input. This is what lets a pilot run auto-follow (position-following) with autothrottle switched off: the switch gates only the two GVARs above, not the position-waypoint stream. Default `-1` (unassigned) means no additional restriction — autothrottle engages purely on lock state and airframe type, matching this spec's behavior for a pilot who doesn't want a dedicated hardware switch. Once assigned, the channel is considered armed while its pulse width sits inside a pilot-configured range, `autothrottleEnableMinThresholdUs`/`autothrottleEnableMaxThresholdUs` (§3.2, §3.5) — a closed range rather than a single hardcoded switch-high threshold, so it can describe a 2-way, 3-way, or 6-pos switch's specific armed position(s).
- The along-track error calculation (§4) that turns "the follower isn't quite where its slot says it should be" into a signed correction term, computed from data `FollowManager::loop()` already has on hand every cycle (the resolved `FollowTarget`, the resolved leader course, the follower's own GPS fix via `GNSSManager`).
- New runtime-configurable tuning fields — proportional gain and min/max speed clamp (§3.5) — persisted and exposed exactly like the rest of `FollowRuntimeConfig`.
- **Stall-margin guidance in the UI, not a dynamic sink-rate correction** (§1.4). `minTargetSpeedMps`'s field description/tooltip in `html/follow.js` tells the pilot to set it comfortably above the airframe's actual stall speed — a margin of roughly a third above stall is offered as a reasonable starting point — since this iteration has no reactive rescue boost to fall back on if the setpoint ever needs to sit near the clamp floor.
- Guidance (not automation) for how a pilot rewires the reference script to consume these two GVARs instead of its RC-channel/local-logic inputs (§6), matching how `2026-08-13`'s §5 handles the equivalent OSD-side wiring. With min/max, feedforward, and correction all resolved before the GVAR is ever written, the INAV-side script collapses to three jobs: read the engage GVAR, feed the setpoint GVAR straight into PID3, and clamp/apply the resulting throttle — see §6 for the (much shorter) resulting wiring and `docs/plans/2026-08-28-FollowSpeedAutothrottle-Plan.md`'s new LC-rewrite section for the exact CLI block.
- **Airframe-type gating.** A new `MSPManager` query of `MSP2_INAV_MIXER`'s `platformType` field (§2.4), cached per-connection the same way `getFCVariant()` already is. The autothrottle engage condition (§3.2) is amended to additionally require `platformType == INAV_PLATFORM_AIRPLANE`; this is enforced in firmware (`FollowManager::loop()`, §3.6/§4.4), not just surfaced in the UI, because a stale/wrong engage flag reaching PID3 on a multirotor would command a nonsensical throttle override with no corresponding INAV-side lever to catch it. `/followmanager/status` and `html/follow.js` also surface the detected platform so the two config fields (§3.5) are greyed out with an explanatory tip when the connected FC isn't a fixed-wing, mirroring the existing INAV-version-gate tip already next to `conditionFlagsGvarIndex`.

### 1.4 Out of scope (this iteration)
- Any change to INAV firmware — same MSP-native, no-firmware-fork posture as `2026-08-13`.
- **Sink-rate/stall protection.** An earlier draft of this spec moved the reference script's sink-rate correction (`LC20`-`LC26`) into FF as a reactive rescue boost reading the follower's own vertical speed. That's deferred to a later phase, not built this iteration. `LC20`-`LC26` are still deleted outright along with the rest of the baseline/trim chain (§2.1, §6) — they are **not** replaced with an FF-side equivalent yet, so this iteration genuinely has no dynamic sink-rate reaction, not "the same protection, relocated." The mitigation for now is configuration discipline: `minTargetSpeedMps` (§3.5) is the safety floor, and both its field description in `html/follow.js` and this spec document it as needing to sit comfortably above the airframe's actual stall speed — a margin of roughly a third above stall is a reasonable starting point — so the clamp that already exists for other reasons absorbs what a reactive boost would otherwise have handled. A pilot who wants an active rescue boost instead of a static margin can revisit this once a later phase is scoped.
- **Pilot manual-trim override.** The reference script's RC12 trim knob (`LC35`-`LC38`) is dropped, not reimplemented as an FF config field or an INAV-side layered nudge — once FF is the setpoint's sole source of truth, there's no longer a natural place for a pilot to interactively override it in flight without reopening exactly the two-sources-of-truth problem this spec is designed to avoid. A pilot who wants this back can still wire their own downstream Logic Condition reading `GVAR(targetSpeedGvarIndex)` and adding to it (§6's last bullet) — FF's contract doesn't forbid it, it just doesn't build or document it as a first-class option anymore. The INAV-side "TARGET" OSD readout is kept (§6), just repointed to display FF's live setpoint instead of the old pilot-trim value, since it's still useful to see what FF is actually commanding.
- Lateral/vertical slot correction beyond what the existing position-waypoint stream already does. This spec only ever adjusts the *speed* lever; the along-track error it reads is a byproduct of the position solve that already exists, not a new control axis.
- **Rover/boat support.** `INAV_PLATFORM_ROVER`/`INAV_PLATFORM_BOAT` are also throttle-driven-speed airframes in principle (§1.2), but the reference script and this spec's control law are untested against them and neither is enabled this iteration — the engage gate (§3.2) checks specifically and only for `INAV_PLATFORM_AIRPLANE`. Broadening the gate to include them is a one-line change if a pilot asks for it, but isn't speculatively built now.
- Helicopter, tricopter, and multirotor support — never throttle-to-speed airframes; permanently out of scope for this control law, not just deferred like rover/boat above.
- Any change to how `peer->gps.groundSpeed` is populated or transmitted — it's already part of every peer's telemetry (`msp_raw_gps_t`, `RadioManager.cpp`); this spec only reads a field that already exists.

---

## 2. Background

### 2.1 What the reference script does today (recap)
See `docs/explainers/inav-airspeed-autothrottle.md` for the full decode. The pieces this spec replaces or interacts with:

| Reference script piece | LC ID(s) | Role | This spec's disposition |
|---|---|---|---|
| `engageSwitchHigh` | LC27 | RC channel 11 > 1480 arms the engage check | **Replaced, and moved off INAV entirely.** Reads `autothrottleEngageGvarIndex` instead (§3.2, §6) — but the RC-channel check itself is no longer an INAV Logic Condition at all; FF now reads the pilot's arm switch directly (`autothrottleEnableRcChannel`, §3.2/§3.5) and folds it into the same GVAR, alongside lock state and airframe type. |
| `engageSwitchEdge` / `autothrottleEngaged` latch | LC28, LC33 | Rising-edge arm, abort-condition disarm | **Simplified.** FF re-asserts the engage flag every cycle already gated on live lock state (and now the live RC arm switch), so the edge-detection latch's job (remembering "the pilot flipped the switch") is no longer needed — see §6's guidance on collapsing this to a level read. |
| `groundSpeedReading` (LC4), `currentGroundSpeed`/GVAR1 (LC50) | LC4, LC50 | PID3's **measurement** input — INAV's own live ground speed | **Untouched.** This is local FC telemetry; FF has no involvement and needs none. |
| Baseline target + sink-rate correction + pilot trim → `targetGroundSpeed`/GVAR0 | LC20-LC38 | PID3's **setpoint** input | **Replaced/removed.** FF computes the baseline + along-track correction directly (§4) and writes it as the setpoint. Pilot trim is dropped, not reimplemented (§1.4). Sink-rate correction is deleted outright, also not reimplemented this iteration (§1.4) — deferred to a later phase. |
| PID3 → throttle override | LC39-46 | Consumes the two GVARs above, drives the servo | **Untouched.** Pure INAV-side consumer of the two GVARs; doesn't know or care that FF is now the source. |

### 2.2 Wire contract (unchanged from `2026-08-13` §2.1)
- `MSP2_INAV_SET_GVAR` (`0x2214`): `{uint8_t index; int32_t value}` on the wire (`MSP.h`'s `msp_set_gvar_t`), but INAV's GVARs themselves are signed 16-bit (`-32768..32767`) — see `2026-08-13` §2.1. Ground speed in cm/s comfortably fits (±327 m/s ceiling, far beyond any FormationFlight airframe).
- One-way, best-effort: a dropped write just leaves the GVAR at its last value until the next one lands. No ack/retry.
- Requires INAV ≥ 9.0.0, gated exactly as `2026-08-13` §2.2 already established (`MSPManager::sendGvar()` already silently no-ops below that version — this spec adds no new version-gating logic, it just calls the same existing `sendGvar()`).

### 2.3 GVAR budget
Per the project's current allocation:
- GVARs 0-3: `FollowManager`'s fixed debug output (`FOLLOW_DEBUG_*_GVAR_INDEX`, `FollowConfig.h`), on only when `config.debug` is set.
- Two pilot-assigned slots from `2026-08-13`: `statusGvarIndex`, `conditionFlagsGvarIndex` (default `-1`, dropdown 0-7 when enabled).
- **This spec's two pilot-assigned slots**: `targetSpeedGvarIndex`, `autothrottleEngageGvarIndex` (same default-`-1`, dropdown-0-7 pattern).

That's all 8 of INAV's GVARs spoken for if a pilot enables every FF feature at once on non-overlapping indices — which is exactly why every one of these fields stays independently pilot-assigned rather than FF claiming fixed indices the way the debug GVARs do (see §3.4 on the collision guard).

### 2.4 Airframe type via `MSP2_INAV_MIXER`
`MSP2_INAV_MIXER` (`0x2010`) is a no-payload request; INAV replies with its current mixer config:

```cpp
struct msp_mixer_config_t {
  uint8_t  motorDirectionInverted;
  uint8_t  reserved;
  uint8_t  motorstopOnLow;
  uint8_t  platformType;        // see InavPlatformType below
  uint8_t  hasFlaps;
  uint16_t appliedMixerPreset;
  uint8_t  maxSupportedMotors;
  uint8_t  maxSupportedServos;
} __attribute__ ((packed));
```

`platformType` (byte offset 3) is INAV's `flyingPlatformType_e`, mirrored here as a new `MSP.h` enum:
```cpp
enum InavPlatformType {
    INAV_PLATFORM_MULTIROTOR = 0,
    INAV_PLATFORM_AIRPLANE   = 1,
    INAV_PLATFORM_HELICOPTER = 2,
    INAV_PLATFORM_TRICOPTER  = 3,
    INAV_PLATFORM_ROVER      = 4,
    INAV_PLATFORM_BOAT       = 5,
};
```
This spec's engage gate (§3.2) checks for exactly `INAV_PLATFORM_AIRPLANE` — see §1.4 for why rover/boat/tricopter/helicopter/multirotor are all excluded this iteration despite some of them nominally having a throttle-to-speed relationship too.

`MSP2_INAV_MIXER` and its `platformType` field predate the `MSP2_INAV_SET_GVAR` floor this spec already inherits (present since INAV 1.9 / MSP API 2.1, versus §2.2's INAV ≥ 9.0.0 floor) — any FC new enough to accept this feature's GVAR writes at all is already new enough to answer this query, so this spec adds no separate version gate for it, the same reasoning `2026-08-13` §2.2 used to justify not re-checking API version per-command.

New `MSPManager` surface (`MSPManager.h`/`.cpp`), mirroring `getFCVariant()`'s cache-once-per-connection pattern (`MSPManager.cpp:87-125`) exactly — queried once after the FC's identity is established and held for the life of the connection, not re-polled every cycle, since a pilot changing mixer platform requires an INAV reboot anyway:
```cpp
// Returns the connected FC's mixer platform type (MSP2_INAV_MIXER, spec
// docs/spec/2026-08-28-FollowSpeedAutothrottle.md §2.4). Cached once per
// connection like getFCVariant(); defaults to INAV_PLATFORM_MULTIROTOR
// (the least permissive answer — never engages autothrottle) until a real
// reply is received, so an unanswered/pre-connection query fails closed
// rather than open.
InavPlatformType getPlatformType();
```

---

## 3. GVAR Contract

### 3.1 `targetSpeedGvarIndex` — the setpoint
- **Value:** target ground speed in **cm/s**, signed. Matches `peer->gps.groundSpeed`'s native unit (`msp_raw_gps_t::groundSpeed`, int16 cm/s) and the reference script's own internal units (`KMH_TO_CMS`-scaled `GVAR0`) — no unit translation needed on the INAV side beyond pointing PID3's setpoint operand at this GVAR instead of its old locally-computed one.
- **Written:** every `FollowManager::loop()` cycle in which a leader is actively locked, a target has been solved (the same success path that calls `sendFollowWaypoint()` today), the connected FC is `INAV_PLATFORM_AIRPLANE`, and the pilot has autothrottle armed via `autothrottleEnableRcChannel` (§3.2) — continuously, like the position waypoint, not change-detected like the two `2026-08-13` status GVARs. A ground-speed setpoint that's mid-correction changes basically every cycle anyway; gating sends on "did the value change" would add complexity for no MSP-traffic savings.
- **Not written** (this cycle) whenever any part of that isn't true — gate inactive, `ACQUIRING`/`LOCKED_HOLDING`, geometry-insane offset, target-too-far, the connected FC isn't `INAV_PLATFORM_AIRPLANE` (§2.4, §3.6), or the RC arm switch is off (§3.2). See §3.2 for why this is safe despite leaving a stale value sitting in the GVAR.

### 3.2 `autothrottleEngageGvarIndex` — the engage flag
- **Value:** `1` while FF is actively commanding a real target this cycle (identical gating to §3.1's "written" condition above), **and** `MSPManager::getPlatformType() == INAV_PLATFORM_AIRPLANE` (§2.4, §3.6), **and** the pilot has autothrottle armed via `autothrottleEnableRcChannel` (below); `0` in every other case, including gate-inactive, `ACQUIRING`, `LOCKED_HOLDING`, geometry-insane, target-too-far, non-airplane platform, and switch-off.
- **The RC arm switch — `autothrottleEnableRcChannel`.** A new pilot-assigned RC channel (§3.5), read by FF itself every cycle, not by an INAV Logic Condition — unlike the reference script's original `LC27` (`rc.channel[11] > 1480`), which ran entirely on the INAV side. It's read the same way `FollowManager`'s existing `rcLongChannel`/`rcLatChannel`/`rcVertChannel` trim axes already read RC input (`MSPManager::getRcChannelUs()`), but resolved to a boolean rather than a continuous offset: `channel1Based < 1` (unassigned) always resolves to armed=true, so a pilot who never sets this field gets exactly this spec's pre-switch behavior (autothrottle gated only by lock state and airframe type); once assigned, the channel's live pulse width is armed while it falls within a pilot-configured **closed range**, `[autothrottleEnableMinThresholdUs, autothrottleEnableMaxThresholdUs]` (§3.5), rather than a single switch-high threshold. A range, with both ends independently configurable, is what lets the same field pair describe a plain 2-way switch (a wide range covering the whole high half of travel), a 3-way switch (a narrow range around one specific middle or high position), or a 6-pos switch (a narrow range around one specific detent) — the pilot picks whichever sub-range of stick travel on their bound channel should mean "armed," rather than FF assuming a 2-position switch shape. This is what lets a pilot run auto-follow (position-following) with autothrottle switched off in flight — the switch gates only this GVAR pair, not the position-waypoint stream, and is read live every cycle with no edge-latch (see the next bullet on why a latch isn't needed).
- **Written every cycle regardless of value**, using the same change-or-heartbeat send rule as `2026-08-13`'s `updateStatusGvars()` (`FOLLOW_GVAR_HEARTBEAT_MS`) — this one *is* a discrete, rarely-changing flag, so the existing dedup logic is the right fit, unlike §3.1's setpoint.
- **This is what makes §3.1's staleness safe.** The reference script's engage switch (`LC27`, rewired per §6 to read this GVAR instead of RC channel 11) gates the entire `autothrottleEngaged` latch. When this flag is `0`, INAV's own PID3-override logic never engages regardless of what stale number sits in `targetSpeedGvarIndex` — the aircraft falls through to whatever throttle behavior INAV's regular `POSHOLD_3D` navigation already provides. This directly implements "autothrottle only engages when you have a leader **and** the pilot has armed it; if either isn't true, iNAV's regular POS HOLD takes over."
- Explicit `0` at startup/reconnect (before any lock has ever been attempted), same rationale as `2026-08-13` §3.1: a reboot mid-flight must not leave a stale `1` sitting in this GVAR with no FF process alive to correct it.

### 3.3 Why two GVARs and not one
The reference script's engage/disengage decision (§2.1's `LC27`/`LC33`) and its setpoint computation (`LC20`-`38`) are two independently-read INAV Logic Conditions today, and stay that way here — collapsing them into one GVAR (e.g., "negative value means disengaged") would make the setpoint's own sign semantics do double duty as an engage flag, which breaks the moment a legitimate target speed needs to reach zero (a leader coming to a stop) or forecloses ever supporting a rearward/negative ground-speed leader. Two orthogonal GVARs, one boolean and one magnitude, is both simpler to reason about and consistent with `2026-08-13`'s own two-orthogonal-GVARs precedent (§3.2 of that spec, for the same reason).

### 3.4 Collision guard
Same posture as `2026-08-13` §3.4: **front-end only**. `html/follow.js`'s existing collision check (guarding `statusGvarIndex`/`conditionFlagsGvarIndex` against each other) must be extended to also cover `targetSpeedGvarIndex`, `autothrottleEngageGvarIndex`, and — whenever `config.debug` is true — the four fixed `FOLLOW_DEBUG_*_GVAR_INDEX` values, since a pilot bench-testing with debug on could otherwise silently alias a debug GVAR onto one of these two. No corresponding firmware-side check is added, matching `2026-08-13`'s explicit precedent that this is a UX guard, not a wire-level invariant.

### 3.5 New config fields
Added to `FollowRuntimeConfig` (`FollowManager.h`) and `FollowEepromRecord` (bumping `FOLLOW_EEPROM_VERSION` 4 → 5), following the exact existing field-by-field pattern:

```cpp
// GVAR indices (§3.1/§3.2), -1 = disabled. Both must be enabled together for
// the feature to do anything meaningful — see §7 open question on whether
// applyConfig() should enforce that pairing.
int16_t targetSpeedGvarIndex = FOLLOW_TARGET_SPEED_GVAR_INDEX;       // default -1
int16_t autothrottleEngageGvarIndex = FOLLOW_AUTOTHROTTLE_ENGAGE_GVAR_INDEX; // default -1

// Pilot arm switch (§3.2), -1 = unassigned. Read the same way
// rcLongChannel/rcLatChannel/rcVertChannel already read RC input
// (MSPManager::getRcChannelUs()), but resolved to a boolean: unassigned
// always resolves armed=true (no restriction, this spec's pre-switch
// behavior); once assigned, the channel's live pulse width is armed while
// it falls within [autothrottleEnableMinThresholdUs,
// autothrottleEnableMaxThresholdUs] below.
int16_t autothrottleEnableRcChannel = FOLLOW_AUTOTHROTTLE_ENABLE_RC_CHANNEL; // default -1

// The armed range itself (§3.2), µs. A closed range rather than a single
// switch-high threshold so the same two pilot-configurable bounds can
// describe a 2-way, 3-way, or 6-pos switch's specific "armed" position(s) —
// see §3.2 for why a single hardcoded threshold can't express that.
int16_t autothrottleEnableMinThresholdUs = FOLLOW_AUTOTHROTTLE_ENABLE_MIN_THRESHOLD_US; // default 1700
int16_t autothrottleEnableMaxThresholdUs = FOLLOW_AUTOTHROTTLE_ENABLE_MAX_THRESHOLD_US; // default 2100

// Proportional gain, (cm/s of setpoint correction) per (meter of along-track
// error) — see §4.2. Chosen in these units specifically so integer values
// are plenty tunable (html/follow.js's number inputs parseInt() before ever
// reaching state, same constraint documented on FollowRuntimeConfig's
// existing geometry fields) without needing fractional gain support.
int16_t speedCorrectionKp = FOLLOW_SPEED_CORRECTION_KP;

// Safety envelope on the final commanded setpoint (§4.3), m/s, human-facing
// like minCourseSpeed — converted to cm/s at the wire site. This is the
// ONLY place a min/max target speed is ever configured — see §4.3/§6: the
// INAV-side script has no bound of its own to drift out of sync with this.
//
// minTargetSpeedMps doubles as this iteration's stall-margin safety net
// (§1.4) in place of a dynamic sink-rate correction: html/follow.js's field
// description should tell the pilot to set this comfortably above the
// airframe's actual stall speed — a margin of roughly a third above stall
// is offered as a reasonable starting point, not a validated/enforced
// figure (airframe-specific, same caveat as every other default in §7).
double minTargetSpeedMps = FOLLOW_MIN_TARGET_SPEED_MPS;
double maxTargetSpeedMps = FOLLOW_MAX_TARGET_SPEED_MPS;
```

`speedCorrectionKp`/`autothrottleEnableRcChannel`/`autothrottleEnableMinThresholdUs`/`autothrottleEnableMaxThresholdUs` are naturally integer-range (an EEPROM `int16_t` with no conversion needed, unlike the `double` geometry fields that get `lround()`-narrowed). `minTargetSpeedMps`/`maxTargetSpeedMps` follow `minCourseSpeed`'s existing `double`-in-RAM/`int16_t`-in-EEPROM pattern exactly.

`applyConfig()` gains matching validation: both GVAR indices in `{-1} ∪ [0,7]` (mirrors the existing `statusGvarIndex` check verbatim); `autothrottleEnableRcChannel` in `{-1} ∪ [1, MSP_MAX_SUPPORTED_CHANNELS]` (mirrors the existing `rcLongChannel` check verbatim); `maxTargetSpeedMps > minTargetSpeedMps >= 0` (mirrors the existing `minCourseSpeed >= 0` check, plus the new ordering constraint since this is a two-sided clamp rather than a single floor). `speedCorrectionKp` has no sign restriction — `0` is a valid, meaningful configuration (feedforward-only, no slot-lag correction; see §7). **Deliberately excluded from `applyConfig()`:** any ordering check between `autothrottleEnableMinThresholdUs` and `autothrottleEnableMaxThresholdUs`. Unlike the speed clamp above, an inverted or degenerate threshold pair here is inert rather than unsafe — `autothrottleArmed()`'s range check (§3.2, §4.6) just never evaluates true, so the switch silently never arms, the same practical failure mode as any other misconfigured RC channel. The ordering guard for this pair lives only in `html/follow.js`'s `validateConfig()`, as a UI-only, advisory nudge — the same posture §3.4 already establishes for the collision guard.

No new config field carries the airframe restriction itself — `INAV_PLATFORM_AIRPLANE` is not pilot-configurable, it's live FC telemetry read via `MSPManager::getPlatformType()` (§2.4) and checked at the point of use (§3.6), the same way `minCourseSpeed`'s gating reads live `peer->gps.groundSpeed` rather than storing a config bit for "is there a leader."

### 3.6 Airframe gate — enforcement points
This spec's engage precondition (§3.2) is enforced at two independent layers, because a wrong engage flag reaching PID3 has real consequences (an unwanted throttle override) and a UI-only guard can't stop a pilot who sets the GVAR indices directly via `POST /followmanager/config`:
- **Firmware (binding).** `FollowManager::loop()`'s existing success-path condition (the one gating `sendFollowWaypoint()`/`updateDebugGvars()` today) is AND-ed with `MSPManager::getPlatformType() == INAV_PLATFORM_AIRPLANE` before it's allowed to produce `engaged=true` for `updateAutothrottleGvars()` (§4.4). A non-airplane FC — or an FC that hasn't answered `MSP2_INAV_MIXER` yet, per `getPlatformType()`'s fail-closed default (§2.4) — takes the exact same code path as any other early-return condition (gate inactive, still acquiring, etc.): `engaged=false`, `autothrottleEngageGvarIndex` written `0`, `targetSpeedGvarIndex` left untouched. This is the binding check; the two below are UX layered on top of it.
- **REST/status.** `FollowManager::statusJson()` gains a `platformType` field (raw `InavPlatformType` value, or `-1`/omitted if no FC connected yet) alongside the `targetSpeedCmS`/`autothrottleEngaged` pair already planned for §5, so `/followmanager/status` reports what firmware actually detected.
- **UI (advisory).** `html/follow.js` reads that new `platformType` status field and greys out `targetSpeedGvarIndex`/`autothrottleEngageGvarIndex`'s dropdowns with an explanatory tip ("Requires a fixed-wing (airplane) mixer on the follower FC — detected platform: {name}") when it's anything other than `INAV_PLATFORM_AIRPLANE`, mirroring the existing INAV-version-gate tip's placement and tone (`html/follow.js`, next to `conditionFlagsGvarIndex`, quoted in §2.3's table context). A pilot can still set the fields while greyed out (same non-blocking posture as `2026-08-13` §3.4's collision guard) — firmware's gate above is what actually matters.

---

## 4. Control Law

### 4.1 Inputs available every `loop()` cycle
By the time `loop()` reaches its success path (past the gate/lock/geometry/target-too-far checks), it already has:
- `peer` — the locked leader, giving `peer->gps.groundSpeed` (int16 cm/s, leader's current ground speed) for free; this is the exact same field `resolveCourseDeg()` already reads for its own min-speed check.
- `courseDeg` — the leader's resolved course (`resolveCourseDeg(peer)`), already computed this cycle.
- `target` — the solved `FollowTarget` (absolute lat/lon of the follower's commanded slot position), already computed via `slotToLatLon()`.
- The follower's own current position, via `GNSSManager::getSingleton()->getLocation()` / `horizontalDistanceTo()` / `courseTo()` — the same calls `updateDebugGvars()` already makes against `target`, just not currently made unconditionally (today they're gated on `config.debug`).

No new peer telemetry, no new radio payload, no new MSP query — everything this control law needs is already resolved earlier in the same `loop()` cycle.

### 4.2 Along-track error
`slotToLatLon()` places the target by rotating a track-relative offset `(long_m, lat_m)` into world-frame `(north_m, east_m)` via course angle `θ`:
```
north_m = long_m·cos(θ) − lat_m·sin(θ)
east_m  = long_m·sin(θ) + lat_m·cos(θ)
```
This is a pure rotation, so it inverts cleanly (`R(θ)⁻¹ = R(−θ)` for a rotation matrix). Given `(north_m, east_m)` as the vector **from the follower's current position to its solved target** — exactly what `GNSSManager::horizontalDistanceTo(targetLoc)`/`courseTo(targetLoc)` already produce, the same pair `updateDebugGvars()` computes today — the track-relative **longitudinal** component of that vector is:
```
alongTrackErrorM = north_m·cos(θ) + east_m·sin(θ)
```
Sign convention: **positive** means the target is *ahead* of the follower along the leader's track — i.e., the follower is lagging its slot and needs to speed up. Negative means the follower is ahead of where its slot currently is and needs to slow down. This needs no new geometry primitive; it's the existing `slotToLatLon()` rotation run in reverse against the existing debug-offset calculation, just made unconditional instead of `config.debug`-gated.

### 4.3 Setpoint
```
targetSpeedCmS = leaderSpeedCmS + speedCorrectionKp · alongTrackErrorM
targetSpeedCmS = clamp(targetSpeedCmS, minTargetSpeedMps·100, maxTargetSpeedMps·100)
```
where `leaderSpeedCmS = peer->gps.groundSpeed` (already int16 cm/s, no conversion) and `alongTrackErrorM` is §4.2's signed meters. The clamp is a hard safety envelope — never command below the airframe's stall speed regardless of how far ahead of its slot the follower is, and never above some sane ceiling regardless of how far behind — evaluated **last**, exactly like the existing altitude-floor clamp (`FOLLOW_MIN_ALT_M`) is applied last against the summed target altitude in today's `loop()`. Per §1.4, `minTargetSpeedMps` is this iteration's entire stall-safety story: there is no dynamic sink-rate/rescue term to fall back on, so this floor needs to be set with a real margin above the airframe's actual stall speed (§3.5's guidance: roughly a third above stall as a starting point), not tuned down to "just above stall."

This is proportional-only by design for the slot-lag term (per the resolved control-law direction, §7 leaves the door open for an integral term later without redesigning this section) — no persistent-bias accumulator, no derivative term on FF's side (PID3 on the INAV side already supplies its own P/I/D/FF against this setpoint vs. the FC's live ground speed; FF's job stops at producing the setpoint, not at closing the loop against the follower's actual speed — that's what PID3 already does).

### 4.6 Where this lives in code
New private helpers, next to `resolveCourseDeg()`/`resolveHeadingDeg()`/`resolveAxisOffset()` in `FollowManager.h`/`.cpp`:
```cpp
// Signed along-track distance (meters) from the follower's current position
// to `target`, in the leader's track frame (spec §4.2) — positive means the
// target is ahead of the follower (follower is lagging its slot).
double resolveAlongTrackErrorM(const FollowTarget &target, double courseDeg) const;

// Combines the leader's live ground speed and §4.2's along-track correction
// into a clamped cm/s setpoint (spec §4.3).
int32_t resolveTargetSpeedCmS(const peer_t *peer, const FollowTarget &target, double courseDeg) const;

// The pilot's autothrottle arm switch (spec §3.2), read the same way
// resolveAxisOffset() already reads rcLongChannel/rcLatChannel/rcVertChannel
// via MSPManager::getRcChannelUs(). Unassigned (< 1) resolves true — no
// restriction, this spec's pre-switch behavior. Otherwise armed while the
// channel's live pulse width falls within
// [autothrottleEnableMinThresholdUs, autothrottleEnableMaxThresholdUs].
bool autothrottleArmed() const;
```
and a GVAR-writing counterpart alongside `updateStatusGvars()`/`updateDebugGvars()`:
```cpp
// Writes autothrottleEngageGvarIndex (spec §3.2) every cycle (change+heartbeat
// gated, like updateStatusGvars()), and targetSpeedGvarIndex (spec §3.1)
// only when engaged — unconditionally, every cycle, like the position
// waypoint stream.
void updateAutothrottleGvars(bool engaged, int32_t targetSpeedCmS);
```
called with `engaged=false` at every one of `loop()`'s existing early-return points (gate inactive, still acquiring/holding, geometry-insane, target-too-far — the same four call sites that already call `updateStatusGvars()`), and, in the success path, `engaged = (MSPManager::getSingleton()->getPlatformType() == INAV_PLATFORM_AIRPLANE) && autothrottleArmed()` (§3.6, §3.2) plus the computed setpoint, alongside the existing `updateDebugGvars()`/`sendFollowWaypoint()` calls — no fifth early-return call site needed, the airframe check and the arm-switch check both fold into the same boolean that already decides `engaged` at that one site.

---

## 5. Status/Debug Reporting

- `FollowManager::statusJson()` gains a `targetSpeedCmS`/`autothrottleEngaged` pair (mirroring how `statusGvarValue`/`conditionFlagsGvarValue` are already reported), for `/followmanager/status` and the web UI — useful for bench-verifying the computed setpoint without needing a GVAR wired to anything on the INAV side yet.
- `FollowManager::statusJson()` also gains `platformType` (§3.6), the raw `InavPlatformType` value `MSPManager::getPlatformType()` last returned — this is what `html/follow.js` reads to grey out §3.5's two config fields on a non-airplane FC.
- No change to the existing four debug GVARs (`FOLLOW_DEBUG_*`) — this feature's own two GVARs already serve the equivalent "what is FF actually commanding" bench-testing purpose for speed, the same way the debug GVARs do for position.

---

## 6. INAV-Side Setup (guidance, not implementation)

Mirrors `2026-08-13` §5's framing: FF documents what's needed; the pilot performs it once in INAV Configurator. **This section changed substantially from an earlier draft** — with min/max, slot-lag correction, and the engage/arm-switch logic now entirely FF-owned (§3.2, §3.5), the INAV-side script's job shrinks from ~40 Logic Conditions to about ten. See `docs/plans/2026-08-28-FollowSpeedAutothrottle-Plan.md`'s LC-rewrite section for the exact, pasteable CLI block — this section covers the *what/why* of each change.

Starting from the reference script's CLI dump:
- **Engage input.** The entire `autothrottleEngaged` chain (`LC27`-`LC33`: RC-channel switch, edge latch, abort-condition OR) collapses to one line: `autothrottleEngaged = GVAR(autothrottleEngageGvarIndex) == 1`. INAV no longer reads any RC channel for this at all — the arm switch moved to FF (§3.2), which folds it into the same GVAR alongside lock state and airframe type before the value ever reaches INAV. FF re-asserts this flag fresh every cycle, so none of the original latch/abort machinery is doing useful work anymore — a pilot who specifically wants an extra local abort belt (GPS validity, landing state) can still AND one in, but it's no longer the default this spec documents.
- **Setpoint input.** PID3's setpoint operand reads `GVAR(targetSpeedGvarIndex)` **directly** — no intermediate GVAR, no baseline, no clamp, no correction on the INAV side at all. `LC20`-`LC26` (baseline + sink-rate correction) and `LC35`-`LC38` (pilot manual trim) are deleted outright, not merely disconnected (§1.4) — leaving them in place, even disabled, is exactly the "looks like it's still configuring something" trap that motivated this rewrite in the first place. `LC20`-`LC26` are deleted, not replaced with an FF equivalent this iteration (§1.4) — this spec's stall-safety story for now is `minTargetSpeedMps` set with a real margin above stall, not a reactive correction.
- **Measurement input no longer needs the GVAR1/`LC4`/`LC50` passthrough chain.** Since that chain's only other purpose (gating on `hasReachedFlyingSpeed`, and — in the original pitot-based design — choosing between two possible sensor sources) no longer has anything downstream depending on it, PID3's measurement operand can read INAV's live ground speed **directly** as a `FLIGHT`-type operand instead of via a GVAR intermediary. (`programming/pid.c`'s setpoint/measurement resolution calls the same generic operand-value function regular Logic Conditions use, which accepts a `FLIGHT` operand — see the plan's LC-rewrite section for confidence notes and a documented fallback if a given INAV build doesn't accept it.)
- **Target OSD readout is kept, repointed.** The original "TARGET" element displayed the pilot's manual trim value; there's no trim anymore, so it's repointed to display FF's live `targetSpeedGvarIndex` value (unit-converted for display), visible whenever `autothrottleEngaged` is true — still useful to see what FF is actually commanding, just no longer interactive.
- **"OVERRIDING" OSD element is removed.** It displayed the old sink-rate-correction latch, which is deleted outright and not replaced this iteration (§1.4) — there's no active sink-rate state on either side to display anymore. Once a later phase adds a reactive sink-rate/rescue term, a corresponding OSD bit could be added at that point — e.g. a new `FollowConditionCode` value on the already-extensible `conditionFlagsGvarIndex` (`2026-08-13` spec §3.2) — but that's out of scope here (§1.4).
- **Throttle output chain is untouched** (`LC39`-`LC46`: PID3 output → offset → halve → clamp to 1250-1800µs servo-pulse range → `OVERRIDE_THROTTLE`). This clamp is an actuator/hardware-range safety bound, not a speed bound — a fundamentally different quantity (µs of throttle pulse, not m/s of ground speed) that isn't convertible 1:1 against `minTargetSpeedMps`/`maxTargetSpeedMps` without knowing the airframe's throttle curve, so there's no duplication to resolve here and no reason to touch it.

---

## 7. Open Questions

- **Should `applyConfig()` require `targetSpeedGvarIndex` and `autothrottleEngageGvarIndex` to be enabled/disabled together?** Enabling only one produces a config that's individually valid but functionally inert or half-built (a setpoint with nothing gating its use, or an engage flag with nothing to engage). `2026-08-13`'s two GVARs are independently useful on their own (a pilot might want lock-state only, or condition-flags only), but this spec's two are a matched pair with no standalone use for either alone — worth deciding whether that asymmetry should be enforced server-side (unlike `2026-08-13`'s deliberately-independent fields) or left as a UI-only nudge (e.g., grey out one dropdown until the other is set), matching this spec's own §3.4 precedent of keeping validation front-end-only.
- **Slew-rate limiting.** §4.3's setpoint can, in principle, jump sharply cycle-to-cycle right after acquiring a lock (large initial along-track error) or after a `LOCKED_HOLDING` → `LOCKED` recovery. The reference script's PID3 already has its own D-term response to a setpoint step, and FF's own emit rate (`emitHz`, default 4 Hz) already bounds how often such a jump can occur — but whether an explicit max-change-per-second limiter on `targetSpeedCmS` is worth the added state (parallel to how `lastKnownGood` freezes the position offset) isn't decided here. Deferred as a possible follow-up refinement, not a blocker for a first implementation.
- **Default values for `speedCorrectionKp`, `minTargetSpeedMps`, `maxTargetSpeedMps`.** All three are airframe-specific (a park-flyer's sane speed envelope and correction aggressiveness differ substantially from a faster fixed-wing platform) — this spec defines the *fields* and their units but leaves picking their compile-time defaults to the implementation/bench-testing stage, the same way `FOLLOW_MIN_COURSE_SPEED`'s `2.0` m/s default was presumably tuned against real flight testing rather than derived analytically. `minTargetSpeedMps` carries extra weight this iteration (§1.4) since it's the only stall-safety mechanism — its default and its UI copy (§3.5's "roughly a third above stall" guidance) are both worth a second look once real airframes are bench-tested against this feature.
- **`FOLLOW_AUTOTHROTTLE_ENABLE_MIN_THRESHOLD_US`/`_MAX_THRESHOLD_US`'s defaults.** §3.2/§3.5's RC arm switch is a new pattern in this codebase — `rcLongChannel`/`rcLatChannel`/`rcVertChannel` read a continuous stick position, not a switch threshold range — so there's no existing convention to copy the bounds from. `1700`-`2100us` is a reasonable placeholder (leaves headroom above a typical switch-low ~1000-1300us range, and covers the whole high end of travel for a plain 2-way switch) but isn't validated against real transmitter/receiver endpoint calibration, and a pilot wiring a 3-way or 6-pos switch will need to narrow the range to their specific channel's calibrated per-position pulse widths regardless of what ships as the default.
- **Reintroducing sink-rate/stall protection.** Deferred to a later phase (§1.4) rather than built here. Once revisited, the open design question is the same shape as the earlier draft's: read the follower's own vertical speed (`MSP_ALTITUDE`'s `estimatedActualVelocity`) and add a rescue term to `resolveTargetSpeedCmS()`'s output before the final clamp — not decided here, just flagged as the natural next increment on top of this iteration's static `minTargetSpeedMps` margin.
- **PID3 measurement reading a `FLIGHT` operand directly.** §6 relies on `programming/pid.c` accepting a `FLIGHT`-type measurement operand (confirmed by source inspection that setpoint/measurement resolution shares the same generic operand-value function Logic Conditions use, but not confirmed against a running INAV instance) — the plan's LC-rewrite section carries a documented GVAR1-passthrough fallback in case a pilot's INAV build behaves differently; worth removing this open question once bench-verified.
- **Bench-testing without a real airplane-mixer FC.** §3.6's fail-closed default (`getPlatformType()` reports `INAV_PLATFORM_MULTIROTOR` — never engages — until `MSP2_INAV_MIXER` actually answers) means the `hexagon-patrol` spoof-peer bench setup (`ae88680`) still needs a real or simulated INAV instance answering that query as `INAV_PLATFORM_AIRPLANE` to exercise this feature at all; a pilot can't bench-test autothrottle engagement against a bare spoof peer with no FC attached the way position-following can be. Whether that's acceptable or whether `MSPManager` needs a debug/override knob to force a platform type for bench testing isn't decided here.
