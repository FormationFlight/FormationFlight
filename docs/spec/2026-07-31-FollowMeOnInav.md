# FormationFlight — Autonomous Follow Mode (Option B) — Engineering Spec

**Status:** Draft for planning — v3, revised against source (see changelog)
**Target firmware:** FormationFlight (ESP32/ESP8266, PlatformIO/C++)
**Follower FC:** INAV, multirotor (quadcopter) only
**Source of truth for the flight controller side:** INAV `GCS NAV` follow-me via MSP `MSP_SET_WP` (#209), special waypoint #255

**Changelog since v1:**
- Corrected §5 field names/scaling/altitude claims and §6.1 emitter code against the actual `PeerManager`/`MSPManager`/`MSP` source (see inline notes).
- Added §6.3: peer selection is now a lock-on-first-detected state machine, not a per-cycle "pick first active" scan (addresses requirement: don't fail over to a second peer if the locked one is lost).
- Added §10: follow parameters are runtime-configurable via the web UI, not compile-time-only (addresses requirement: web UI configurability). This supersedes v1's "config may be compile-time initially" framing.

**Changelog since v2:**
- Added §7.6: an absolute, configurable altitude floor (`FOLLOW_MIN_ALT_M`) clamping the final commanded altitude, independent of §7.4's leader-relative vertical-separation rule. Closes a gap where a low/landing/descending leader (or a `BELOW` slot) could command the follower to zero or negative home-relative altitude. Found in review — **not yet implemented in the shipped Phase 1 code** as of this revision; see the plan's post-Phase-1 addendum.

**Changelog since v3:**
- Split §10 (web UI configuration) into two independent stages, matching the plan's Phase 3/Phase 4 split: **live, in-memory config** (edits apply immediately, lost on reboot, no `ConfigHandler` dependency) and **explicit EEPROM persistence** (a distinct "commit" action, gated on fixing the pre-existing `ConfigHandler` reset bug). Previously §10 described a single combined "edit + auto-persist" flow; that's no longer accurate to the plan and is corrected here. See §10.1 for the rationale and §§10.3–10.4 for the two designs.

**Changelog since v4:**
- Added §7.7: nose/heading control. The follower can now command its own heading independently of its direction of travel, using the same `MSP_SET_WP` #255 message §4/§6.1 already stream — no second MSP command, no new INAV flight mode. Driven by a new `FOLLOW_HEADING_MODE` key (§9), runtime-editable via the web UI like every other §9 key (§10.3), defaulting to `POINT_LEADER`. §4's `p1` field, previously always `0`, is now load-bearing — see §7.7 and the updated §4 table. Applies uniformly regardless of follower airframe (rotorcraft or fixed-wing) — §7.7 explains why no craft-type branch is needed.

**Changelog since v5:**
- Added a fifth heading mode, `COURSE_RELATIVE`, to §7.7/§9: like `FIXED`, but the configured degrees value is added to the leader's live course instead of being an absolute compass heading, so the commanded nose angle rotates with the leader instead of staying fixed to north. Renamed the §9 config key from `FOLLOW_HEADING_FIXED_DEG` to `FOLLOW_HEADING_DEG` since it's now shared between `FIXED` (absolute) and `COURSE_RELATIVE` (offset) — its meaning is mode-dependent, not mode-specific.

---

## 1. Purpose & Scope

### 1.1 Goal
Add a mode to the FormationFlight (FF) firmware such that, when the pilot engages a switch on the follower aircraft, the follower autonomously flies to and holds a configurable position relative to a peer aircraft ("the leader"), using the peer position data FF already receives over its radio link. Position is expressed as a 3D offset (distance + relative location: behind, right, above, etc.).

### 1.2 In scope
- Modifying FF firmware to emit `MSP_SET_WP` (#255) to the follower's FC.
- Reading the leader's position from FF's existing peer table.
- Gating the emitter on a pilot-controlled switch / flight mode.
- A configurable follow-geometry section (distance + relative location).
- Freshness and sanity safety guards inside FF.
- **Single-leader lock-on behavior:** when multiple peers are visible, follow only the first peer detected at the moment follow mode is engaged; do not automatically retarget to a different peer if the locked one is lost (§6.3).
- **Runtime configuration via the web UI:** all follow parameters (geometry, safety bounds, trigger mode, target-peer selection, nose-heading mode) are readable and writable from FF's existing web UI. Two independent stages (§10): live edits apply immediately and take effect without a reflash, but are lost on reboot unless explicitly persisted via a separate "save permanently" (commit) action, which is what actually survives reboot.
- **Nose-heading control (§7.7):** the follower can be commanded to point its nose along its direction of travel, at the leader, or at a fixed compass heading, independently of the position it's flying to — addresses formation flying where the follower's direction of travel and desired nose orientation differ (most relevant to rotorcraft, which can translate without turning).

### 1.3 Out of scope (this iteration)
- Fixed-wing followers. The follower is assumed to be a multirotor that can hold and reposition. Fixed-wing trail flight is explicitly deferred.
- Multi-follower coordination / collision avoidance between multiple followers.
- Leader-side changes. The leader only needs to be broadcasting position over FF as it does today.
- Any change to INAV firmware. We use INAV's existing `GCS NAV` mechanism unmodified.
- **Automatic failover to an alternate peer** when the locked leader is lost. This is a deliberate non-goal, not an oversight — see §6.3 for the rationale (predictability/safety over availability).

### 1.4 Assumptions
- Both aircraft already run INAV + FF and can see each other in the HUD (peer telemetry is working end-to-end).
- The follower's FC has a working GPS 3D fix, calibrated compass, and flies `NAV POSHOLD` cleanly on its own.
- FF on the follower is connected to the FC over an MSP-configured UART (the same link used today for the radar HUD).

---

## 2. Background / Why This Design

- **FF is display-only today.** FF relays peer position/altitude/speed/name to the FC purely for OSD/HUD display. It does not command navigation. There is no stock path from peer data into the follower's navigation controller.
- **INAV already has a follow-me hook: `GCS NAV`.** With the FC in `NAV POSHOLD` + `GCS NAV`, an external consumer repeatedly writes special waypoint **#255** (the POSHOLD target) via `MSP_SET_WP` (#209). INAV flies to that moving point. This is the mechanism we drive.
- **Modify FF firmware** because everything needed already lives in FF: the peer table, the follower's own position, and the MSP-writing plumbing (FF already frames and writes MSP to the FC, including GPS injection via `MSP2_COMMON_SET_RADAR_POS`). We add one outbound message and a small decision task; no extra UART or companion computer.
- **FF's config today is compile-time only**, set via PlatformIO `build_flags` in `targets/*.ini`. There is no working runtime-persisted config and no config-write web endpoint anywhere in FF (verified — see §10.2). Making follow parameters web-configurable (§1.2) is new infrastructure, not a reuse of an existing mechanism, and splits into two independently-shippable stages (§10.1): **live, in-memory editing** (no dependency on the EEPROM bug below) and **EEPROM persistence** of those edits, which does depend on fixing a pre-existing EEPROM bug (§10.2) as a prerequisite.

---

## 3. High-Level Architecture

```
                 FF radio link (LoRa / ESP-NOW)
   Leader FF  ───────────────────────────────────▶  Follower FF (ESP32)
                                                      │
                                    ┌─────────────────┼────────────────────┐
                                    │ PeerManager      │ MSPManager        │
                                    │  peer table      │  MSP TX/RX to FC  │
                                    └─────────┬────────┴─────────┬─────────┘
                                              │                  │
                                        [A] peer pos       [B] local alt / [C] switch state
                                              │                  │
                                          ┌───▼──────────────────▼────┐
                                          │   follow module (NEW)     │
                                          │  - gate on switch/mode    │
                                          │  - lock peer, freshness   │
                                          │  - compute 3D slot target │
                                          │  - emit MSP_SET_WP #255 │
                                          └───────────┬───────────────┘
                                                      │ MSP_SET_WP (#209, wp=255)
                                                      ▼
                                            Follower INAV FC
                                     (NAV POSHOLD + GCS NAV active)
```

The pilot's switch position engages `NAV POSHOLD` + `GCS NAV` on the FC. FF detects the follow condition, locks onto a leader, computes the target slot behind/around it, and streams it to WP#255. The FC flies there and holds.

**Runtime pattern note (verified against source):** FF has **no task-scheduler abstraction**. `src/main.cpp`'s `loop()` calls each manager singleton's `.loop()` unconditionally every iteration, in a fixed order (`RadioManager` → `WiFiManager` → `PeerManager` → `GNSSManager` → `MSPManager`), and each manager self-throttles internally with a `millis()` check (e.g. `PeerManager::loop()` gates its body on a 100 ms interval; `MSPManager::loop()` gates on a `nextSendTime` tied to the LoRa TDMA slot). The new follow module should be a new singleton (e.g. `FollowManager`) added as one more `.loop()` call at the end of that sequence — after `PeerManager`, `GNSSManager`, and `MSPManager` have all run so a fresh peer, fresh self-location, and open MSP link are available that iteration — self-gated the same way at `1000 / FOLLOW_EMIT_HZ` ms, and skipped until `sys.phase > MODE_OTA_SYNC` (mirroring `MSPManager::loop()`'s own gate).

---

## 4. Follow-Me Flight-Controller Contract (INAV side, fixed)

These values are dictated by INAV and must not be changed:

- **Modes required active on follower:** `NAV POSHOLD` **and** `GCS NAV` together.
- **Message:** `MSP_SET_WP`, command **209**.
- **Waypoint number:** **255** (POSHOLD/follow-me slot).
- **action:** **1** (`MSP_NAV_STATUS_WAYPOINT_ACTION_WAYPOINT`, already `#define`d in `src/lib/MSP/MSP.h:410`). Setting action = 0 makes the message invalid.
- **lat / lon:** `int32`, degrees × **1e7**.
- **alt:** `int32`, **centimeters**. Home-relative when p3 bit0 = 0.
- **p1:** still sent as **heading, in whole degrees, or `0` for "don't update heading"** (`p1` otherwise means cruise speed cm/s on ordinary mission waypoints 1–60) — but **as of §7.7's 2026-08-17 correction, this write is currently inert** for a follower in `NAV POSHOLD_3D`; it never reaches the yaw-rate PID on INAV 9.x regardless of value. Kept as a forward-compatible best-effort write (free — same message, no extra MSP traffic) in case INAV ever extends `POSHOLD_3D` to honor it. The command that actually works today is the separate `MSP_SET_HEAD` (§7.7).
- **p2, p3, flag:** all **0** for our use (p3 bit0 = 0 ⇒ altitude relative to home; do not set AMSL).

Payload layout (21 bytes, little-endian) — **this struct already exists in FF's codebase**, verbatim, at `src/lib/MSP/MSP.h:670-680` as `msp_set_wp_t` (packed):

| Offset | Field           | Type   | Value                         |
|-------:|-----------------|--------|--------------------------------|
| 0      | waypointNumber  | uint8  | 255                            |
| 1      | action          | uint8  | 1                              |
| 2      | lat             | int32  | latitude × 1e7                 |
| 6      | lon             | int32  | longitude × 1e7                |
| 10     | alt             | int32  | altitude in cm (home-relative) |
| 14     | p1              | int16  | heading in degrees (1–360), or 0 for "no heading update" — see §7.7 |
| 16     | p2              | int16  | 0                               |
| 18     | p3              | int16  | 0                               |
| 20     | flag            | uint8  | 0                               |

MSPv1 frame: `$ M < [len=21] [cmd=209] [payload...] [crc]`. FF already has the framing/CRC/ACK-wait plumbing for this in `MSP::command()` (`src/lib/MSP/MSP.cpp:244`) — do not hand-roll a new frame writer (see §6.1).

---

## 5. Data Sources Inside FF (integration seams) — verified against source

Three seams bind to real symbols in the current FF checkout, confirmed by reading `PeerManager`, `MSPManager`, and `MSP`.

### [A] Leader position — PeerManager
File: `src/lib/Peers/PeerManager.h`/`.cpp`. The peer record is `peer_t` (`PeerManager.h:9-29`). **Correction vs. v1 of this spec:** there are no top-level `lat`/`lon`/`latRaw`/`lonRaw`/`groundCourse`/`relativeAltitude`/`age` fields on `peer_t`. Those names are **web-JSON-only**, produced on the fly by `PeerManager::statusJson()` (`PeerManager.cpp:193-234`). The real internal fields firmware code must use are:

| Internal field                  | JSON name           | Type    | Notes |
|----------------------------------|----------------------|---------|-------|
| `peer->gps.lat`, `peer->gps.lon` | `lat`/`lon`, `latRaw`/`lonRaw` | `int32_t` | see scaling note below |
| `peer->gps.groundCourse`         | `groundCourse`       | `int16_t` | **degrees × 10**, not plain degrees — divide by 10 before use (precedent: `MSP_GNSS.cpp:38`, `loc.groundCourse = rawLocation.groundCourse / 10;`) |
| `peer->gps.groundSpeed`          | `groundSpeed`        | `int16_t` | cm/s |
| `peer->relalt`                   | `relativeAltitude`   | `int16_t` | leader alt minus follower alt, meters (computed in `PeerManager::loop()`, `PeerManager.cpp:162`, from two `GNSSLocation.alt` doubles — **not home-relative**, just a raw GPS/baro altitude difference) |
| `peer->updated`                  | `updated`             | `uint32_t` | millis() timestamp |
| `peer->lost`                     | `lost`                | `uint8_t`  | set to 2 when `millis() - peer->updated > LORA_PEER_TIMEOUT` (`main.h:51`, **6000 ms**) |
| `peer->id`                       | `id`                  | `uint8_t`  | LoRa slot id, assigned once, stable for the session (see §6.3 for lock semantics and its caveat) |
| (none — computed only in JSON)   | `age`                 | —       | `millis() - peer->updated`; no stored equivalent exists yet |

- **⚠ Scaling gotcha (confirmed, and the exact factor differs from what v1 assumed):** `peer->gps.lat`/`.lon` are `int32_t` scaled **×1e6** in practice. The struct's own header comment (`msp_raw_gps_t`, `MSP.h:369-370`) claims ×1e7, but the working code that actually uses this field for outbound MSP — `MSPManager::sendRadar()` (`MSPManager.cpp:219-220`) — does `position.lat = peer->gps.lat * 10; // x 10E7`, i.e. it treats the stored value as ×1e6 and multiplies by 10 to reach INAV's native ×1e7. **The follow emitter must do the same `* 10`**, mirroring `sendRadar()`'s proven behavior, not the (incorrect) struct comment. Getting this wrong scales the target ~10× toward 0°/0° → guaranteed flyaway. This is the single most safety-critical fact in the spec; verify empirically on the bench (§11.1 item 4) before any flight regardless.
- **Peer selection is no longer "scan every cycle for the first active peer."** See §6.3 — it's a lock-on-acquire state machine.

### [B] Follower's own altitude — MSPManager
**Correction vs. v1:** v1 claimed FF "already fetches" a home-relative self-altitude and this could be reused. **Confirmed false — no such stored value exists anywhere in FF today.** `MSPManager::getLocation()` (`MSPManager.cpp:135-149`) does an uncached `MSP_RAW_GPS` request, and `msp_raw_gps_t.alt` is meters, typically GPS/MSL — not home-relative, not cached. The struct that actually holds INAV's home/baro-relative estimate, `msp_altitude_t` (`estimatedActualPosition`, cm, `MSP.h:275-279`) and its command `MSP_ALTITUDE` (`#define MSP_ALTITUDE 109`, `MSP.h:51`), **exist but are never requested anywhere in the current codebase** (zero call sites). This is new work: add a poll of `MSP_ALTITUDE` in `MSPManager` and cache `estimatedActualPosition` as `local_altitude_cm()`.

### [C] Switch / mode state — MSPManager
Two supported triggers, as in v1, but **simplify the "preferred" one against what already exists:**

1. **AUX-channel (simple first cut):** genuinely new — FF does not poll `MSP_RC` anywhere today (only the unused `#define MSP_RC 105` / `msp_rc_t` struct exist). Would need new polling + an AUX accessor.
2. **GCS-NAV-reactive (preferred, safer):** **simpler than v1 assumed.** `MSP::getActiveModes(uint32_t *activeModes)` (`MSP.cpp:304-330`) already implements the classic `MSP_STATUS` + `MSP_BOXIDS` flow and is already called today by `MSPManager::getState()` (`MSPManager.cpp:29-38`) to read the ARM bit for OSD/HUD status. The same bitmap already includes `MSP_MODE_NAVPOSHOLD` (bit 9) and `MSP_MODE_GCSNAV` (bit 23) — both `#define`d in `MSP.h:96,110`. **Drop the `MSP2_INAV_STATUS` idea from v1 entirely**; there's no need for an INAV-specific v2 message. Triggering on "GCS NAV active" is a one-line `bitRead(activeModes, MSP_MODE_GCSNAV)` addition reusing plumbing that's already proven in production for the arm-state case.

Ship #1 to get flying; move to #2 for the production behavior (unchanged recommendation from v1, now cheaper to implement than v1 assumed).

---

## 6. New Firmware Components

### 6.1 MSP emitter (add to MSPManager)
**Correction vs. v1:** the v1 code sample hand-rolled a byte buffer and an inline XOR checksum. Both already exist in FF as reusable, tested infrastructure — `msp_set_wp_t` (§4) and `MSP::command()` (`MSP.cpp:244`, which frames, computes the checksum, and writes for you). Use them instead of duplicating:

```cpp
// MSP_SET_WP (#209) — INAV follow-me special waypoint #255.
// Requires NAV POSHOLD + GCS NAV active on the follower FC. headingDeg (p1)
// is still sent as of the §7.7 correction above, but is currently inert on
// INAV 9.x for a follower in NAV POSHOLD_3D — kept as a forward-compatible
// best-effort write in case INAV extends POSHOLD_3D to honor it; see
// sendSetHead() for the mechanism that actually works today.
void MSPManager::sendFollowWaypoint(int32_t lat_1e7, int32_t lon_1e7, int32_t alt_cm, int16_t headingDeg) {
    msp_set_wp_t wp{};
    wp.waypointNumber = 255;
    wp.action = MSP_NAV_STATUS_WAYPOINT_ACTION_WAYPOINT; // must be 1
    wp.lat = lat_1e7;
    wp.lon = lon_1e7;
    wp.alt = alt_cm;         // home-relative, p3 bit0 = 0 below
    wp.p1 = headingDeg; wp.p2 = 0; wp.p3 = 0;
    wp.flag = 0;
    msp->command(MSP_SET_WP, &wp, sizeof(wp));
}

// MSP_SET_HEAD (#211) — §7.7 correction: the actual heading path. Callers
// must gate on MSPManager::isHeadingHoldActive() (INAV's HEADING HOLD box) —
// see §7.7 for why the write is otherwise a silent no-op on the FC.
void MSPManager::sendSetHead(int16_t headingDeg) {
    msp_set_head_t head{};
    head.magHoldHeading = headingDeg;
    msp->command(MSP_SET_HEAD, &head, sizeof(head));
}
```

### 6.2 Follow task (new module `follow`)
Registered as a self-gated `.loop()` per §3, targeting **~4 Hz** (see §8 rate caution).

```cpp
void FollowManager::loop() {
    if (!follow_switch_active())   { peerLock.clear(); return; }   // [C] gate — also drops any lock (§6.3)
    peer_t *p = peerLock.resolve();                                // [A] peer lock/acquire (§6.3)
    if (!p || peer_is_stale(p))    return;                         // freshness guard (§7.4/§8) — HOLD, do not re-target

    // Resolve configured slot -> track-relative meters (§7 geometry)
    FollowOffset o = follow_resolve_offset();           // long/lat/vert meters
    LL tgt = slotToLatLon(p->gps.lat * 10, p->gps.lon * 10, p->gps.groundCourse / 10.0,
                          o.longitudinal_m, o.lateral_m);

    // NOTE on frame mixing: local_altitude_cm() is the follower's baro/GPS-fused
    // home-relative estimate (MSP_ALTITUDE); p->relalt is a raw-GPS-only delta
    // (MSP_RAW_GPS altitude, leader minus follower — PeerManager.cpp). Summing
    // them is the best available approximation given the leader only ever
    // broadcasts raw GPS telemetry (§1.3 rules out leader-side changes), but the
    // result's real accuracy is bounded by GPS vertical error (commonly ~8 m per
    // receiver), not by the FC's more accurate baro-fused estimate. This is why
    // FOLLOW_MIN_VSEP_M (§7.4) carries an explicit GPS-error margin, not just a
    // physical-clearance buffer.
    int32_t alt_cm = local_altitude_cm()                 // [B] home-relative, baro/GPS-fused
                   + (int32_t)(p->relalt * 100)           // leader vs follower, raw-GPS delta, m->cm
                   + (int32_t)(o.vertical_m * 100);       // configured vertical offset

    // Hard floor (§7.6): never command the follower below a configurable
    // minimum home-relative altitude, regardless of what the leader-relative
    // offset math produced above — e.g. the leader flying low/landing, or a
    // BELOW slot dragging the follower toward the ground. Clamp, don't
    // reject: the follower should keep tracking laterally and hold at the
    // floor rather than stop emitting entirely.
    if (alt_cm < FOLLOW_MIN_ALT_CM) alt_cm = FOLLOW_MIN_ALT_CM;

    if (!follow_target_sane(p, tgt, alt_cm)) return;    // sanity bounds (§7.4)

    // Nose heading (§7.7, corrected 2026-08-17). Resolves to the 1-360 wire
    // value directly (0 => FOLLOW_HEADING_OFF, "don't send a heading");
    // computed the same way regardless of whether the follower is a
    // rotorcraft or fixed-wing (§7.7 — no craft-type branch).
    int16_t headingDeg = follow_resolve_heading_deg(p, courseDeg);

    // headingDeg also rides along in p1 here — currently inert on INAV 9.x
    // for a follower in NAV POSHOLD_3D (kept forward-compatible; see §7.7 and
    // MSPManager::sendFollowWaypoint()'s comment), NOT the live mechanism.
    mspManager->sendFollowWaypoint(tgt.lat_1e7, tgt.lon_1e7, alt_cm, headingDeg);

    // The independent MSP_SET_HEAD send below is what actually steers the
    // aircraft today — gated on INAV's HEADING HOLD box being active (§7.7
    // correction explains why that gate, not p1 above, is load-bearing).
    if (headingDeg != 0 && mspManager->isHeadingHoldActive()) {
        mspManager->sendSetHead(headingDeg);
    }
}
```

### 6.3 Peer Selection & Lock State Machine (NEW — addresses "follow only the first-detected aircraft")

**Requirement:** when FF is tracking multiple peer aircraft and the pilot enables follow mode, the follower must lock onto and follow only the **first** peer detected at that moment. If the locked peer is subsequently lost, the follower must **not** automatically retarget to a different peer — it falls back to the standard stale-peer procedure (§8: stop emitting, FC holds last WP#255) until either the same locked peer's telemetry is regained, or the user explicitly changes the target-peer setting (or cycles the follow switch).

This is a deliberate design choice, not an omission: silently retargeting to a different aircraft mid-flight would change the follower's commanded position without any pilot action, which is exactly the kind of surprise behavior a formation-flying safety design should avoid. Predictability (stay locked, hold if lost) is preferred over availability (always chase *some* peer).

**State machine** (owned by a new `PeerLock` helper inside the follow module):

| State | Entered when | Behavior |
|---|---|---|
| `IDLE` | Follow gate (switch/`GCS NAV`) inactive | No lock held. `follow_update()` no-ops. |
| `ACQUIRING` | Gate just became active, no lock yet | Scan the peer table once; lock onto the **first** peer with `id > 0` and not `lost` (or, if `FOLLOW_TARGET_PEER` names an explicit id, lock onto that id once it's present and fresh). Locking stores `peer->id` (and `peer->name` as a sanity cross-check — see caveat below), not an array index. |
| `LOCKED` | A peer is locked and its telemetry is fresh | Emit WP#255 tracking that specific peer id every cycle, per §6.2. |
| `LOCKED_HOLDING` | The locked peer's telemetry goes stale (`peer_is_stale()`, §8) or `lost` | **Stop emitting.** Do not scan for or switch to another peer. FC holds the last commanded WP#255 (standard §8 procedure). Keep checking the *same* locked id for freshness every cycle. |
| → back to `LOCKED` | The same locked peer id becomes fresh again | Resume emitting to that peer without re-acquiring — this is "regaining" the original leader, not picking a new one. |
| → `IDLE` | Gate goes inactive (switch off, or `GCS NAV` drops) | Clear the lock unconditionally. Re-engaging always re-runs `ACQUIRING` and can lock a different (or the same) first-detected peer. |
| → `ACQUIRING` | User changes `FOLLOW_TARGET_PEER` (via web UI, §10) while gate is active | Clear the lock and re-acquire immediately under the new setting. This is the explicit "user changes a setting" escape hatch. |

**Caveat (flag, don't silently patch around):** `peer->id` is a small LoRa slot id assigned once via `pick_id()` and is stable for as long as that peer keeps its slot, but if a peer drops out long enough that FF reassigns its slot to a *different* physical aircraft, the same `id` could now refer to someone else. Mitigate by also checking `peer->name` still matches what was locked before resuming emission from `LOCKED_HOLDING`; if the name changed under the same id, treat it as a lost lock (fall through to `IDLE`-style safe behavior) rather than silently following a new aircraft under the old id. This should be confirmed against real multi-peer bench testing (§11.1) before flight.

`FOLLOW_TARGET_PEER = FIRST_ACTIVE` (default) uses the "first peer seen at acquire time" rule above. `FOLLOW_TARGET_PEER = <peer id>` (settable via web UI once peers are visible, §10) skips the "first" ambiguity entirely by pinning acquisition to a specific id from the start; the hold-on-loss / no-failover behavior is identical either way.

---

## 7. Follow Geometry (configurable distance + relative location)

This is the configurable positioning section. The follower's slot is a 3D offset from the leader, expressed in the **leader's track-relative frame**, so the formation slot rotates with the leader's heading (the way real formation flight works).

### 7.1 Coordinate frame (authoritative definition)
Origin = leader's current position. Axes:

- **Longitudinal (x):** along the leader's `groundCourse`. **+ = ahead** (direction of travel), **− = behind**.
- **Lateral (y):** perpendicular to course. **+ = right** (starboard, 90° clockwise from course), **− = left** (port).
- **Vertical (z):** **+ = above**, **− = below**.

A slot is the triple `(longitudinal_m, lateral_m, vertical_m)`.

### 7.2 Geometry math (reference implementation)
θ = leader `groundCourse` in **plain degrees** (clockwise from north) — remember to divide FF's internal `peer->gps.groundCourse` by 10 first (§5[A]).

```cpp
struct LL { int32_t lat_1e7, lon_1e7; };

// long_m: +ahead/-behind ; lat_m: +right/-left
LL slotToLatLon(int32_t plat_1e7, int32_t plon_1e7, float course_deg,
                float long_m, float lat_m) {
    double lat = plat_1e7 / 1e7, lon = plon_1e7 / 1e7;
    double th  = radians(course_deg);
    double north_m = long_m * cos(th) - lat_m * sin(th);   // ahead unit + right unit
    double east_m  = long_m * sin(th) + lat_m * cos(th);
    double dlat = north_m / 111320.0;
    double dlon = east_m  / (111320.0 * cos(radians(lat)));
    return { (int32_t)lround((lat + dlat) * 1e7),
             (int32_t)lround((lon + dlon) * 1e7) };
}
```

Sanity checks of the frame (leader heading north, θ=0): behind ⇒ due south; right ⇒ due east; front-right ⇒ northeast. Vertical is applied separately in `alt_cm` (§6.2).

### 7.3 Configuration model

**Canonical config = three signed meter values** (everything else expands to these):

| Key                     | Type  | Meaning                         |
|-------------------------|-------|---------------------------------|
| `FOLLOW_OFS_LONG_M`     | float | + ahead / − behind the leader   |
| `FOLLOW_OFS_LAT_M`      | float | + right / − left of the leader  |
| `FOLLOW_OFS_VERT_M`     | float | + above / − below the leader    |

**Friendly grid (convenience layer)** — pick one enum per axis plus a gap distance per axis; it expands to the canonical meters. This is what expresses "above right", "bottom left", "trail", etc., and is also what the web UI (§10) presents as the primary editing surface.

| Key                    | Values                     | Expands to                               |
|------------------------|----------------------------|-------------------------------------------|
| `FOLLOW_SLOT_LONG`     | `AHEAD` / `CENTER` / `BEHIND` | `+gap_long` / `0` / `−gap_long`       |
| `FOLLOW_SLOT_LAT`      | `LEFT` / `CENTER` / `RIGHT`   | `−gap_lat` / `0` / `+gap_lat`         |
| `FOLLOW_SLOT_VERT`     | `BELOW` / `LEVEL` / `ABOVE`   | `−gap_vert` / `0` / `+gap_vert`       |
| `FOLLOW_GAP_LONG_M`    | float                      | longitudinal spacing when AHEAD/BEHIND   |
| `FOLLOW_GAP_LAT_M`     | float                      | lateral spacing when LEFT/RIGHT          |
| `FOLLOW_GAP_VERT_M`    | float                      | vertical spacing when ABOVE/BELOW        |

**Default slot:** `FOLLOW_SLOT_LONG = BEHIND`, `FOLLOW_SLOT_LAT = CENTER`, `FOLLOW_SLOT_VERT = ABOVE` (i.e. `chase-high`, resolving to `(−H, 0, +V)`). Chosen as the factory default because it keeps the follower clear of the leader's downwash/rotor disc while still being testable with the leader stationary on the ground — a purely horizontal default (e.g. `trail`) would fly the follower into the ground at the leader's altitude when the leader isn't airborne, whereas `BEHIND/CENTER/ABOVE` gives useful separation on the bench and in early flight tests regardless of whether the leader is flying or grounded.

**Named preset examples** (using per-axis gaps; horizontal `H`, vertical `V`):

| Descriptor        | LONG   | LAT    | VERT   | Resulting slot (long, lat, vert)   |
|--------------------|--------|--------|--------|-------------------------------------|
| `trail`           | BEHIND | CENTER | LEVEL  | (−H, 0, 0)                         |
| `lead`            | AHEAD  | CENTER | LEVEL  | (+H, 0, 0)                         |
| `line-abreast-right` | CENTER | RIGHT | LEVEL | (0, +H, 0)                        |
| `line-abreast-left`  | CENTER | LEFT  | LEVEL | (0, −H, 0)                        |
| `echelon-rear-right` | BEHIND | RIGHT | LEVEL | (−H, +H, 0)                       |
| `echelon-rear-left`  | BEHIND | LEFT  | LEVEL | (−H, −H, 0)                       |
| `above-right`     | CENTER | RIGHT  | ABOVE  | (0, +H, +V)                        |
| `bottom-left`     | CENTER | LEFT   | BELOW  | (0, −H, −V)                        |
| `overhead`        | CENTER | CENTER | ABOVE  | (0, 0, +V)                         |
| `chase-high`      | BEHIND | CENTER | ABOVE  | (−H, 0, +V)                        |

Resolution order at runtime: if the friendly grid keys are set, expand them into `FOLLOW_OFS_*_M`; otherwise use `FOLLOW_OFS_*_M` directly. `follow_resolve_offset()` returns the resolved triple.

### 7.4 Geometry safety rules
- **Minimum 3D separation.** Reject/never-arm a slot whose magnitude `sqrt(long² + lat² + vert²)` is below `FOLLOW_MIN_SEP_M`. This forbids the degenerate `CENTER/CENTER/LEVEL` (collision) slot. The web UI (§10) must enforce this client-side too (block Save, not just firmware-side reject) so a bad config never gets persisted in the first place.
- **Minimum vertical gap for stacked slots.** If both horizontal components are ~0 (overhead/underneath), require `|vert| ≥ FOLLOW_MIN_VSEP_M` to avoid a vertical GPS-noise collision. **Default raised to 13 m** (5 m intended physical/collision clearance + 8 m worst-case GPS vertical error budget), because `alt_cm`'s accuracy is bounded by raw GPS vertical error, not by the FC's baro-fused estimate — see §6.2's note on altitude-frame mixing. Do not treat this value as pure physical clearance; it's absorbing measurement noise as well as providing separation.
- **Runtime sanity (`follow_target_sane`).** Reject the computed target if the leader's reported distance is implausibly large, if the solved target is unreasonably far from the follower, or if `groundCourse` is unusable (e.g. leader effectively stationary — see §7.5).

### 7.5 Leader-stationary / low-speed handling
`groundCourse` is meaningless when the leader is nearly stationary, so a track-relative slot cannot be oriented. When leader `groundSpeed` < `FOLLOW_MIN_COURSE_SPEED` (e.g. 2 m/s), fall back to one of:
- **Hold last valid course** (freeze the slot orientation from the last time speed was valid), or
- **Compass/world-frame offset** (treat lateral/longitudinal as fixed N/E) as a configured fallback.
Default: hold last valid course. Configurable via `FOLLOW_STATIONARY_MODE`.

**⚠ Unit gotcha:** `FOLLOW_MIN_COURSE_SPEED` is expressed in m/s (human-facing, matches the web UI), but `peer->gps.groundSpeed` is `int16_t` **cm/s** (§5[A]). Comparing them directly compares e.g. `200 < 2`, which almost never trips — silently defeating this fallback. Convert at the comparison site:
```cpp
if (p->gps.groundSpeed < (int16_t)(FOLLOW_MIN_COURSE_SPEED * 100)) { /* stationary fallback */ }
```

### 7.6 Absolute altitude floor (hard floor, home-relative) (NEW — addresses "protect against the leader/follower altitude going too low")

Distinct from §7.4's vertical-separation rule, which governs the offset *relative to the leader* (e.g. requiring a minimum vertical gap for a stacked slot). §7.6 is an absolute clamp on the **final commanded altitude**, protecting against the leader itself flying low, descending, or landing while a follower is engaged. Without it, a follower configured with a `BELOW` vertical slot — or simply trailing a leader that descends toward or below the follower's own home elevation — could be commanded to `alt_cm <= 0`, i.e. at or below home altitude, or even negative. That is a direct flight-into-terrain risk, not a formation-geometry nicety.

- **`FOLLOW_MIN_ALT_M`** (config, §9): minimum home-relative altitude, in meters, that the follower will ever be commanded to. Human-facing (matches the web UI, mirrors the `FOLLOW_MIN_COURSE_SPEED` pattern above); converted to `FOLLOW_MIN_ALT_CM` (`= FOLLOW_MIN_ALT_M * 100`) at the comparison site in `alt_cm`'s home-relative centimeter frame.
- **Applied as a clamp, not a reject.** If the fully-summed `alt_cm` (§6.2 — follower's own altitude + leader's relative altitude + configured vertical offset) falls below the floor, replace it with the floor value before emitting. Do **not** suppress the whole waypoint the way `follow_target_sane()`'s other checks do. The follower keeps tracking the leader's lateral (lat/lon) position and simply holds at the floor altitude instead; only the vertical component is overridden. Suppressing the entire waypoint instead would leave the follower holding its *last* commanded position indefinitely — not obviously safer than holding at a known, configured-safe minimum altitude, and inconsistent with every other term in §6.2's altitude sum being a plain additive quantity rather than a pass/fail gate.
- **Ordering:** applied after all three altitude terms are summed (§6.2) and before `follow_target_sane()`'s other checks run, so the floor is the last word on the vertical component regardless of which upstream term (follower's own altitude, leader's relative altitude, or the configured vertical offset) caused the low value.
- **Default: 3 m.** Low enough to stay out of the way of normal formation-flight altitudes, but enough to keep the follower clear of ground effect/small obstacles at most sites — a deliberately conservative, small default rather than a guess at "safe cruise altitude." Configurable per-site via the web UI (live editing in §10.3; persisted per-site via §10.4) since the appropriate floor height depends on local terrain/obstacles.
- **Orthogonal to `FOLLOW_MIN_VSEP_M` (§7.4).** The vertical-separation rule prevents the follower's *configured slot* from sitting too close to the leader vertically. The altitude floor prevents the *final commanded altitude* from being too close to the ground, independent of the leader's own altitude or the configured offset. Both checks run; neither substitutes for the other.
- **Does not replace FC-side protections.** This is a floor on what FF *commands* over MSP, not a substitute for INAV's own failsafe/RTH/ground-proximity behavior on the follower's FC — those remain the authoritative last line of defense (consistent with §8's "FC mode is authoritative" framing).

### 7.7 Nose / heading control (NEW — addresses "nose doesn't track direction of travel / leader")

For a fixed-wing follower, nose orientation and direction of travel are the same thing by construction (bank-to-turn flight). For a rotorcraft, they aren't — a multirotor can translate in any direction without yawing, so §6's position-only WP#255 stream leaves the nose wherever it last was (pilot stick input / whatever heading it happened to have), unrelated to the direction it's actually flying. This section adds independent control over the commanded heading.

**Mechanism (original, now superseded — see correction below): `p1` on WP#255 itself.** Verified against INAV firmware source (`navigation.c`, `setWaypoint()`), the WP#255 special-case handler reads `p1` as a heading:

```c
// INAV navigation.c, setWaypoint(), wpNumber == 255 branch
if (wpData->p1 > 0 && wpData->p1 < 360) {
    waypointUpdateFlags |= NAV_POS_UPDATE_HEADING;
}
setDesiredPosition(&wpPos.pos, DEGREES_TO_CENTIDEGREES(wpData->p1), waypointUpdateFlags);
```

`p1` (whole degrees) is written straight to `posControl.desiredState.yaw` when nonzero and in range — this part of the original analysis was correct as far as it went. What it missed: writing `desiredState.yaw` is necessary but **not sufficient** for the value to reach the motors. See the correction below.

**`p1` range gotcha (still accurate background, no longer load-bearing):** the firmware condition is `p1 > 0 && p1 < 360` — both ends exclusive. `p1 == 0` means "leave heading alone," and `p1 == 360` is *also* rejected by the same check. A computed heading that rounds to exactly 0°/360° was sent as `1` instead. `resolveHeadingDeg()` (`FollowManager.cpp`) still performs this wrap today, but now to avoid colliding with its own `0` sentinel, not because of `p1`'s restriction — see the correction below.

**Correction (2026-08-17): `p1` never actually worked for a multirotor follower, and the fix is `MSP_SET_HEAD`, not `p1`.** Field testing on INAV 9.x showed the commanded heading was never honored. Re-verified against INAV firmware source (`navigation.c`, `navigation_multicopter.c`, `flight/pid.c`) turned up the missing half of the picture:

- Writing `posControl.desiredState.yaw` (via `p1`) only reaches the yaw-rate PID controller when `getHeadingHoldState()` (`pid.c`) returns `HEADING_HOLD_ENABLED`. That function checks `navigationGetHeadingControlState()` (`navigation.c`) first, which returns `NAV_HEADING_CONTROL_AUTO` — the condition that would make our `desiredState.yaw` write matter — **only if the FSM state's `stateFlags` include `NAV_REQUIRE_MAGHOLD`**.
- `NAV_STATE_POSHOLD_3D_IN_PROGRESS` — the nav state `isGCSValid()` requires, i.e. the *only* state follow-me ever runs in — does **not** carry `NAV_REQUIRE_MAGHOLD` (`navigation.c`'s FSM state table; contrast with the RTH/WAYPOINT/CRUISE states, which do). So `navigationGetHeadingControlState()` always returns `NAV_HEADING_CONTROL_NONE` for a follower in follow-me flight, and `p1`'s write to `desiredState.yaw`, however correctly formed, was structurally inert — it could never reach the motors via this path, independent of the `(0, 360)` range gotcha above.
- `getHeadingHoldState()`'s only other path to `HEADING_HOLD_ENABLED` is `ABS(rcCommand[YAW]) == 0 && FLIGHT_MODE(HEADING_MODE)` — INAV's own **HEADING HOLD** box (`BOXHEADINGHOLD`, historically called "MAG") must be active on the follower, assigned to an AUX switch, independent of and in addition to `NAV POSHOLD`/`GCS NAV`.

**Mechanism (current): `MSP_SET_HEAD` (#211), sent only while the HEADING HOLD box is active — plus `p1` kept as a forward-compatible best-effort write.** `MSP_SET_HEAD`'s handler (`fc_msp.c`) is unconditional — `updateHeadingHoldTarget(tmp_u16)`, no `isGCSValid()`-style gate, no `(0, 360)` range check — so it's a strictly more direct write than `p1` ever was, decoupled from the position stream. But since the *consuming* side (the yaw-rate PID) still requires `HEADING_HOLD_ENABLED` exactly as above, FF must check the box is active before sending, or the send is a silent no-op on the FC. `MSPManager::isHeadingHoldActive()` reads this off the existing `getActiveModes()`/`MSP_STATUS` bitmap FF already polls for `isGCSNavActive()` (`MSP_MODE_MAG`, bit 4, `MSP.h`) — no new MSP traffic, same plumbing as §5[C]. `MSPManager::sendFollowWaypoint()` **still takes `headingDeg` and still writes it to `p1`** — deliberately, even though it's inert today, because it costs nothing (same message, no extra MSP round trip) and means FF doesn't need a code change if a future INAV extends `POSHOLD_3D` to honor `p1` the way RTH/WAYPOINT/CRUISE already do. `FollowManager::loop()` additionally calls `MSPManager::sendSetHead()`, gated on `isHeadingHoldActive()` and on `resolveHeadingDeg()` not returning its own `0`/`FOLLOW_HEADING_MODE = OFF` sentinel — this second, gated send is what actually steers the aircraft today.

**Operational implication, not just a firmware detail:** unlike `NAV POSHOLD`/`GCS NAV` (which FF's status endpoint already surfaces as a precondition), the HEADING HOLD box is the pilot's/operator's responsibility to wire to an AUX switch and enable on the follower before flight — FF cannot turn it on remotely. Any heading mode other than `OFF` is a silent no-op in the air until that's done. Worth surfacing in `statusJson()`/the web UI as a visible precondition, not just documented here (tracked, not yet implemented as of this correction).

**`FOLLOW_HEADING_MODE` (config, §9), default `POINT_LEADER`:**

| Mode | Commanded heading | Computation |
|---|---|---|
| `OFF` | none (`p1 = 0`, today's pre-§7.7 behavior) | — |
| `COURSE` | direction of travel | `resolveCourseDeg()` (§6.2/§7.5) — the same leader-course value (with its low-speed/stationary fallback) already used to orient the slot itself |
| `POINT_LEADER` | bearing toward the leader's live position | `GNSSManager::courseTo(leaderLoc)` (`GNSSManager.cpp:167-171`) — **already exists**, computes bearing from the follower's own current location to an arbitrary target; no new geometry code needed. Note this points at the *leader*, not at the computed slot target — correct for "look at the leader while flying beside them," which a firmware-side `NAV_POS_UPDATE_BEARING` (bearing-to-target-position) could not do, since the target position is the follower's own offset slot, not the leader. |
| `FIXED` | a configured absolute compass heading | `FOLLOW_HEADING_DEG` (config, §9), used as-is |
| `COURSE_RELATIVE` | a configured offset from the leader's live course | `resolveCourseDeg()` **+** `FOLLOW_HEADING_DEG` (config, §9), wrapped — same course input as `COURSE` mode, but with the configured degrees added as an offset rather than commanding the raw course itself |

Default is `POINT_LEADER` rather than `OFF` or `COURSE`: it's the more generally useful behavior for formation flying (nose toward the other aircraft regardless of the follower's own slot/travel direction) and degrades gracefully — a follower flying directly behind/ahead the leader with `POINT_LEADER` ends up pointing roughly along its direction of travel anyway.

**`FIXED` vs. `COURSE_RELATIVE`:** both consult the same `FOLLOW_HEADING_DEG` config value — the difference is the reference frame, not the parameter. `FIXED` commands that value as an absolute compass heading, unaffected by the leader's course (useful e.g. for a fixed camera/sensor orientation regardless of formation movement). `COURSE_RELATIVE` commands `resolveCourseDeg() + FOLLOW_HEADING_DEG`, so the nose holds a constant *angle relative to the direction of travel* — e.g. `+90` keeps the nose pointed 90° right of course continuously as the leader (and thus the track-relative slot, §7.1) turns, useful for line-abreast/echelon slots (§7.3) where "look outward/sideways relative to the formation's direction of travel" should track turns the way `FIXED`'s compass-locked value cannot. `FOLLOW_HEADING_DEG` is a single signed degrees value; its sign/range convention (`FIXED`: `[0, 360)` compass; `COURSE_RELATIVE`: any signed offset, wrapped the same way as every other computed heading in this section) is the same field regardless of which mode is active — only one mode is active at a time, so there's no ambiguity about which interpretation applies.

**Fixed-wing followers: no special-casing.** `follow_resolve_heading_deg()` runs identically regardless of airframe — there is no craft-type branch, and none is added. This is deliberate, not an oversight:
- Checked against the actual FW control loop (`navigation_fixedwing.c`, `updatePositionHeadingController_FW()`): outside of `NAV_COURSE_HOLD_MODE` and the FW landing glide/flare states — neither of which co-occurs with normal `GCS NAV` follow flight — the FW controller **ignores** `posControl.desiredState.yaw` entirely and self-computes its own bearing to the live position target (`calculateBearingToDestination(&virtualDesiredPosition)`). So under normal follow-me conditions, writing `p1` on a fixed-wing follower has **no effect on the flight path** — it's a safe no-op, not a hazard.
- One cosmetic side effect: `desiredState.yaw` also feeds `navDesiredHeading` (`navigation.c:4434`), a telemetry/OSD "nav target heading" field — so a fixed-wing follower's OSD could transiently display our computed heading as a "nav target" even though it isn't steering to it. Cosmetic only.
- §1.3 currently scopes followers to multirotors only; fixed-wing followers are out of scope for this iteration regardless of §7.7. Not special-casing the heading write here is what keeps §7.7 correct without rework if/when that scope changes, rather than adding craft-type logic now for a case that's already excluded.
- Not verified: whether some INAV build/config could have `NAV_COURSE_HOLD_MODE` active concurrently with `GCS NAV` follow — an unusual combination, flagged in §13 as worth confirming empirically rather than assumed impossible.

---

## 8. Timing, Rate, and Failsafe

- **Emit rate:** ~4 Hz. Do not flood MSP; INAV has limited buffering and can drop messages. Peer updates arrive only a few times per second regardless.
- **Freshness guard (`peer_is_stale`):** the locked peer is stale if `millis() - peer->updated > FOLLOW_PEER_TIMEOUT_MS` (default 1500 ms) **or** `peer->lost` is set. **This is intentionally tighter than FF's own general peer-loss threshold** (`LORA_PEER_TIMEOUT = 6000 ms`, `main.h:51`, which drives the HUD's "lost" flag) — 6 s of staleness is far too old to keep commanding a moving formation slot, so the follow module runs its own tighter check directly against `peer->updated` rather than waiting for `peer->lost` to flip. Never re-command a stale point.
- **Stop = hold, then FC recovers.** When emission stops (stale peer → `LOCKED_HOLDING`, §6.3), INAV holds the last WP#255. The real recovery is the pilot's switch: flipping it drops `GCS NAV` on the FC regardless of FF, reverting to plain `POSHOLD`/manual. The FC mode is authoritative. This is also the mechanism behind §6.3's "no automatic peer failover" rule — losing the leader degrades to a hold, not a retarget.
- **Manual override:** a distinct, always-available switch/mode must let the pilot take control instantly. This is an FC-side concern (standard INAV mode setup), reaffirmed here as a requirement.
- **Startup:** emitter must not send until (a) follower has 3D fix + valid home, (b) a fresh valid peer exists, and (c) the gate is active.

---

## 9. Configuration Summary (all keys)

These are the parameters that exist both as compile-time defaults (`build_flags`, per §2) **and** as runtime-editable values via the web UI (§10) — the web UI is the primary way these get changed after initial flashing. Runtime edits take effect immediately (in-memory) but, per §10's two-stage design, only survive reboot once explicitly persisted via the commit action (§10.4); the compile-time values matter as factory defaults / first-boot (or post-reset) EEPROM seed.

Geometry (§7.3): `FOLLOW_OFS_LONG_M`, `FOLLOW_OFS_LAT_M`, `FOLLOW_OFS_VERT_M`, or grid `FOLLOW_SLOT_LONG/LAT/VERT` + `FOLLOW_GAP_LONG_M/LAT_M/VERT_M` (default slot: `BEHIND`/`CENTER`/`ABOVE`, i.e. `chase-high` — testable with the leader on the ground).

Behavior / safety:
- `FOLLOW_TRIGGER_MODE` = `AUX` | `GCSNAV` (§5[C])
- `FOLLOW_AUX_CH`, `FOLLOW_AUX_US` (when trigger = AUX)
- `FOLLOW_TARGET_PEER` = `FIRST_ACTIVE` | `<peer id>` — governs §6.3's lock-acquire rule; changing this at runtime forces a fresh acquire (§6.3)
- `FOLLOW_EMIT_HZ` (default 4)
- `FOLLOW_PEER_TIMEOUT_MS` (default 1500)
- `FOLLOW_MIN_SEP_M`, `FOLLOW_MIN_VSEP_M` (default **13 m** — 5 m physical/collision clearance + 8 m worst-case GPS vertical error budget; see §7.4)
- `FOLLOW_MIN_COURSE_SPEED` (m/s; converted to cm/s at comparison time against `peer->gps.groundSpeed` — see §7.5), `FOLLOW_STATIONARY_MODE` = `HOLD_COURSE` | `WORLD_FRAME`
- `FOLLOW_MAX_TARGET_DIST_M` (runtime sanity bound)
- `FOLLOW_MIN_ALT_M` (default **3 m**, home-relative; converted to `FOLLOW_MIN_ALT_CM` at comparison time — absolute floor clamping the final commanded altitude, independent of `FOLLOW_MIN_VSEP_M`; see §7.6)
- `FOLLOW_HEADING_MODE` = `OFF` | `COURSE` | `POINT_LEADER` | `FIXED` | `COURSE_RELATIVE` (default **`POINT_LEADER`**) — commanded nose heading, sent via WP#255's `p1` (§7.7); applies uniformly to rotorcraft and fixed-wing followers, no craft-type distinction (§7.7)
- `FOLLOW_HEADING_DEG` (degrees; only consulted when `FOLLOW_HEADING_MODE = FIXED` or `COURSE_RELATIVE`; absolute compass heading for `FIXED`, offset added to the leader's live course for `COURSE_RELATIVE` — same field, mode-dependent meaning; §7.7)

---

## 10. Web UI Configuration (Runtime) (NEW — addresses "follow parameters configurable via web UI")

### 10.1 Two-stage design: live edit vs. persist
Web UI configurability splits into two independently-shippable stages, not one combined "edit + auto-persist" flow:

1. **Live, in-memory config** (§10.3): edits made in the web UI take effect immediately — the running `FollowManager` picks them up on its next `loop()` cycle — but live only in RAM. A reboot always reverts to the compile-time `build_flags` defaults. This stage has **no dependency on EEPROM or the `ConfigHandler` bug** (§10.2), so it can ship and be used for bench/flight tuning on its own.
2. **EEPROM persistence** (§10.4): a distinct, explicit "commit"/"save permanently" action flushes whatever is currently in the in-memory struct to EEPROM, so it survives reboot. This stage **does** depend on fixing the pre-existing `ConfigHandler` reset bug (§10.2) first.

Rationale: the two stages have genuinely different risk profiles and prerequisites (RAM writes are cheap and safe to ship immediately; EEPROM writes are rate-limited, wear-sensitive, and blocked on a fix to shared, non-follow-specific config code). Decoupling them means the live-editing half of the "web UI configurability" requirement (§1.2) doesn't have to wait on the EEPROM prerequisite, and the two halves can be tested and shipped separately.

### 10.2 Why persistence specifically is new work, not a reuse
Verified against source: FF's config today is **compile-time only in practice**, and nothing existing can be reused for EEPROM persistence without a fix first.
- `src/main.h` defines a global `config_t cfg` with a handful of unrelated fields (`force_gs`, `lora_nodes`, `slot_spacing`, `lora_timing_delay`, `msp_after_tx_delay`, `display_enable`) — no follow-related fields, and no generic key/value or JSON layer.
- `config_init()` in `src/lib/ConfigHandler.cpp:27-46` reads `cfg` from EEPROM but then **unconditionally overwrites it with hardcoded defaults** every boot, due to `if (true || cfg.version != VERSION_CONFIG || forcedefault)` (`ConfigHandler.cpp:37`) — a literal `true ||` that short-circuits the version check. **Any EEPROM-persisted edit to `cfg` is discarded on every boot today.** This is a pre-existing bug, unrelated to follow mode, but it directly blocks persisting follow config the same way — **fixing it (removing `true ||`) is a prerequisite for §10.4 (persistence)**, not optional polish. It is **not** a prerequisite for §10.3 (live, in-memory config), which never touches EEPROM.
- There is no HTTP endpoint anywhere in `WiFiManager.cpp` that writes/persists config. All existing `POST` endpoints are either fire-and-forget actions (`/peermanager/spoof`, `/radiomanager/radio_set_enabled`, `/system/reboot`) or spoof/debug setters that live only in RAM (`/gnssmanager/spoof`) — the RAM-only pattern of `/gnssmanager/spoof` is actually the closest existing precedent for §10.3's live-edit endpoint.
- `html/main.js` has a `Settings()` component (lines ~163-199) that *looks* like a config UI (fields for `log_enabled`, `log_level`, `brightness`, `device_name`) but is vestigial: its save handler issues a `GET /system/status` with the JSON body **commented out**, and none of those fields exist in the actual `/system/status` response. It is not a usable pattern to extend as-is, though its layout/component structure is a reasonable starting point for a new panel.

### 10.3 Design — live, in-memory config
- **New `FollowManager` runtime config struct**, holding every key in §9, distinct from the existing unrelated `config_t cfg` (don't overload that struct — it's small, unrelated, and already fragile per §10.2's bug). RAM-only: seeded from the compile-time `build_flags` defaults at boot, mutated in place by writes below. No EEPROM interaction at this stage.
- **New endpoints in `WiFiManager.cpp`**, following the existing per-manager pattern exactly:
  - `GET /followmanager/config` — returns current config as JSON (all §9 keys, resolved values — i.e. reflects grid→canonical expansion so the UI can show both views).
  - `POST /followmanager/config` — accepts JSON body with any subset of §9 keys, validates (including the §7.4 minimum-separation check and the §7.6 altitude-floor value being a sane non-negative number, server-side, not just client-side), updates the in-RAM struct only, and returns the resolved config back (so the UI can confirm what actually took effect after validation). Does **not** touch EEPROM — see §10.4 for that.
  - `GET /followmanager/status` — separate from config: live state for the panel to show while flying/bench-testing — current `PeerLock` state (`IDLE`/`ACQUIRING`/`LOCKED`/`LOCKED_HOLDING`), which peer id/name is locked (if any), gate active/inactive, and the last computed target (for the bench test in §11.1).
- **New web UI panel**, modeled on the existing `Settings()` component's structure in `html/main.js` but wired to the new endpoints (not the dead `/system/status` path): friendly-grid dropdowns (§7.3) as the primary editing surface with an "advanced" toggle to edit raw `FOLLOW_OFS_*_M`, trigger-mode selector, and a target-peer selector populated live from `/peermanager/status` (so the pilot can pick a specific visible peer by name instead of only `FIRST_ACTIVE`) — this doubles as the escape hatch referenced in §6.3's "user changes a setting" transition. The panel visibly indicates edits are session-only (e.g. a banner/badge) until §10.4's persistence lands and a commit succeeds. No debounce needed on this endpoint — an in-RAM write is cheap enough to fire on every change; debounce is only introduced in §10.4 where it guards the EEPROM-write path.
- **Resolution order at this stage:** compile-time `build_flags` values seed the in-memory struct at every boot (there is no persisted value yet to compete with); once §10.4 lands, a persisted EEPROM value — if present — wins instead.
- **§7.7's `FOLLOW_HEADING_MODE`/`FOLLOW_HEADING_DEG` are §9 keys like any other** — no separate endpoint or special-cased UI flow. They're read/written through the same `GET`/`POST /followmanager/config` pair and appear in the same panel as a mode dropdown (`OFF`/`COURSE`/`POINT_LEADER`/`FIXED`/`COURSE_RELATIVE`) plus a single degrees field shown whenever `FIXED` or `COURSE_RELATIVE` is selected, following the existing panel's conditional-field pattern (the grid-vs-advanced offset toggle already does this). The field's label/units should change with the mode (e.g. "Heading (absolute °)" for `FIXED` vs. "Heading offset from course (°)" for `COURSE_RELATIVE`) so the same underlying value isn't misread as the wrong frame — this is a UI-copy concern, not a data-model one, since both modes share `FOLLOW_HEADING_DEG`.

### 10.4 Design — EEPROM persistence
**Depends on:** §10.2's `ConfigHandler` bug fix, and §10.3's in-memory struct already existing (this stage persists it, it doesn't introduce a new one).
- Persist the §10.3 struct via the same EEPROM primitives `config_save()`/`config_init()` use (`ConfigHandler.cpp`), in its own EEPROM region (offset after `cfg`'s footprint).
- **New endpoint:** `POST /followmanager/commit` — takes no body (or an empty one); flushes whatever is currently in the §10.3 in-memory struct to EEPROM. Deliberately separate from `POST /followmanager/config` (§10.3) so a live edit and a "make it permanent" action are two distinct, explicit steps — a pilot can tune live all session without ever touching EEPROM, and only commits when satisfied. Rate-limited/debounced server-side (e.g. reject or coalesce a burst of rapid commit calls) so repeated clicks or an automated caller can't hammer EEPROM with writes — this is where the debounce concern from earlier drafts of this spec actually applies (see §13).
- **New web UI control:** a "Save permanently" button/action distinct from §10.3's live-apply controls, wired to `/followmanager/commit`. Reflects persisted-vs-unsaved state (e.g. disabled once the in-memory struct already matches the last successful commit) and clears §10.3's "session-only" banner once a commit succeeds.
- **Resolution order once this stage exists:** compile-time `build_flags` values are the factory defaults used only to seed EEPROM the first time (or after a config reset); once `/followmanager/commit` has been called at least once, the persisted value wins on every subsequent boot, overriding the compile-time seed. This mirrors how `cfg`/`config_save()` would work correctly once §10.2's bug fix lands.

### 10.5 Non-goals for this section
- No auth on these endpoints, consistent with the rest of FF's local-AP-only web UI (`WiFi.softAP`, no credentials checked on other endpoints either) — not introducing a new security posture here.
- No remote/cloud config sync. Persistence is local EEPROM only, same trust boundary as the rest of FF.
- No automatic/implicit persistence. §10.3's live edits never silently reach EEPROM on their own (e.g. on a timer, or on gate-deactivate) — only the explicit commit action in §10.4 does.

---

## 11. Files / Modules to Change (feeds the plan)

1. **`src/lib/MSP/MSPManager.{h,cpp}`** — add `sendFollowWaypoint()` using the existing `msp_set_wp_t` + `MSP::command()` (§6.1, not a hand-rolled frame; §7.7 extends its signature with `headingDeg`); add `MSP_ALTITUDE` polling + cached `local_altitude_cm()` (§5[B] — confirmed new, not reuse); extend the existing `getActiveModes()`-based state read to expose a "GCS NAV active" accessor (§5[C] — one-line addition to existing code); if using AUX trigger, add new `MSP_RC` polling + accessor.
2. **`src/lib/Peers/PeerManager.{h,cpp}`** — expose a lookup-by-id accessor (peer table is index-based today, not id-keyed — needed for §6.3's lock-by-id) and a `peer_is_stale(peer_t*)` helper using `peer->updated` (§8). No struct changes needed — `peer->gps.lat/lon/groundCourse/groundSpeed` and `peer->relalt` already carry what's needed (§5[A]).
3. **`follow` (new module, e.g. `src/lib/Follow/FollowManager.{h,cpp}`)** — `loop()` (§6.2, including the §7.6 altitude-floor clamp and the §7.7 heading resolve/emit), `PeerLock` state machine (§6.3), `follow_resolve_offset()`, `slotToLatLon()`, `follow_switch_active()`, `follow_target_sane()`, `follow_resolve_heading_deg()` (§7.7 — reuses the existing `resolveCourseDeg()` for `COURSE` mode and `GNSSManager::courseTo()` for `POINT_LEADER`, no new geometry primitive needed), plus a `statusJson()`/config accessors for §10's endpoints.
4. **`src/main.cpp`** — register `FollowManager::getSingleton()->loop()` in the main `loop()` sequence, after `PeerManager`/`GNSSManager`/`MSPManager`, gated the same way `MSPManager` is (§3).
5. **`src/lib/ConfigHandler.cpp`** — fix the `if (true || ...)` bug (`ConfigHandler.cpp:37`) as a prerequisite for §10.4's persistence to work at all (not needed for §10.3's live-edit stage); this is a pre-existing defect, worth flagging to the team as possibly wanted/known before "fixing" it out from under other config.
6. **`src/lib/WiFi/WiFiManager.cpp`** — add `/followmanager/config` (GET+POST, in-memory only, §10.3) and `/followmanager/status` (GET) endpoints first, following the existing per-manager handler pattern; add `/followmanager/commit` (POST, EEPROM write, §10.4) as a later, independent addition once item 5 lands. Optionally extend `/peermanager/spoof` or add a parametrized alternative (lat/lon/course, modeled on `/gnssmanager/spoof`'s param handling) to support §11.1's bench tests.
7. **`html/main.js`** (or a new component file) — new Follow config panel wired to the live-edit endpoints first (§10.3), with a "Save permanently" control added once `/followmanager/commit` exists (§10.4); replaces/ignores the vestigial `Settings()` component's dead save path. §7.7 adds a heading-mode dropdown + conditional fixed-degrees field to this same panel — no new panel/endpoint.
8. **Config / target `.ini` files** — add all §9 keys as `build_flags` defaults (factory-default seed values for §10's first-boot EEPROM init), including §7.7's `FOLLOW_HEADING_MODE`/`FOLLOW_HEADING_FIXED_DEG`.

---

## 12. Test & Acceptance

### 12.1 Bench (props OFF, before any flight)
Existing peer-spoofing is more limited than v1 assumed: `POST /peermanager/spoof` (`PeerManager::enableSpoofing`) exists and injects a **fixed synthetic ring of 5 peers** 100 m apart around self — it does not accept an arbitrary lat/lon/course per request. (There is no `/mspmanager/spoof` — that endpoint does not exist.) For repeatable per-preset geometry verification, either extend the spoof mechanism to accept lat/lon/course params (mirroring `/gnssmanager/spoof`'s existing param handling, `WiFiManager.cpp:111-127`) or verify geometry against the fixed 5-peer ring's known positions.

Verify:
1. Emitter fires **only** when the gate is active (switch/mode).
2. **Peer-lock behavior (§6.3):** with the 5-peer spoof ring active, enabling follow mode locks onto exactly one peer (the first with `id > 0`/not lost) and continues tracking only that id even as other peers update. Killing updates for the locked peer (while others keep updating) causes a transition to `LOCKED_HOLDING` (emission stops, no switch to another peer) — confirm via `/followmanager/status`. Restoring the same peer's updates resumes emission without changing which id is tracked. Toggling the follow switch off/on re-acquires (possibly a different first-detected peer).
3. For each named preset in §7.3, the computed WP#255 lands in the correct place relative to the locked peer's position and `groundCourse`. Read it back with `MSP_WP` (#254) and/or observe in the INAV Configurator / OSD.
4. Altitude reference is correct (target alt tracks follower-home-relative frame; vertical offset applies with correct sign).
5. Coordinate scaling verified: target is at the leader, **not** ~10× displaced — confirm the `* 10` conversion from `peer->gps.lat` (§5[A]) lands correctly.
6. Freshness guard: killing the spoof stops emission within `FOLLOW_PEER_TIMEOUT_MS`.
7. Geometry guards: a sub-minimum-separation config refuses to arm (both firmware-side and web-UI-side, §7.4); stationary-leader fallback behaves as configured — spoof the leader at ~1.5 m/s and ~3 m/s bracketing the `FOLLOW_MIN_COURSE_SPEED` default and confirm the fallback triggers/doesn't at the correct threshold (the cm/s-vs-m/s comparison in §7.5 is easy to get silently wrong).
8. Altitude accuracy: with two real GPS units (not spoofed) at a known, measured height difference, confirm the commanded `alt_cm` lands within `FOLLOW_MIN_VSEP_M` of the intended offset — quantifies the real-world GPS-vertical-error budget the 13 m default (§7.4) is meant to absorb; §12.1 item 4 above only checks `local_altitude_cm()` in isolation, not the combined leader+follower altitude math.
9. **Web UI config, live edit only (§10.3):** editing a preset/offset/trigger-mode/target-peer in the web panel takes effect immediately (watch §2's status endpoint/OSD); rebooting the follower **without** committing confirms the value reverts to the compile-time default (expected — nothing has been persisted yet). Confirm changing `FOLLOW_TARGET_PEER` while the gate is active forces a re-acquire per §6.3.
9a. **Web UI config, persistence (§10.4):** repeat a live edit, then invoke the "Save permanently" commit action; rebooting the follower this time confirms the value survived (didn't revert to compile-time default). Confirm rapid repeated commits don't produce excessive EEPROM writes (the rate-limit/debounce from §10.4 actually engages).
10. **Altitude floor (§7.6):** with a `BELOW` vertical slot (or `ABOVE`/`LEVEL` plus a spoofed leader at/near the follower's home altitude), drive the leader's altitude down until the summed `alt_cm` (§6.2) would go at or below `FOLLOW_MIN_ALT_M`. Confirm the commanded altitude never drops below the floor, and — critically — that the waypoint is still emitted with the floored altitude and correct lat/lon (i.e. this clamps, it does not silently stop emission like a `follow_target_sane()` rejection would). Also confirm `FOLLOW_MIN_VSEP_M` (§7.4) and `FOLLOW_MIN_ALT_M` (§7.6) are independently exercisable — a config that satisfies one should not be assumed to satisfy the other.
11. **Nose heading (§7.7), per mode, read back via `MSP_WP` (#254) or INAV Configurator's WP list / OSD heading indicator:**
    - `OFF`: confirm `p1 == 0` on every emitted WP#255 (byte-identical to pre-§7.7 behavior).
    - `COURSE`: spoof the leader on a known course; confirm commanded heading matches `resolveCourseDeg()`'s value, including its low-speed/stationary-fallback behavior (§7.5) — a leader below `FOLLOW_MIN_COURSE_SPEED` should hold the last-valid course, not jitter.
    - `POINT_LEADER`: with the follower and spoofed leader at known, distinct lat/lons, confirm the commanded heading matches the true bearing follower→leader, independent of the follower's own slot/travel direction (verify specifically in a lateral, e.g. `line-abreast`, slot where course and bearing-to-leader clearly differ).
    - `FIXED`: confirm the commanded heading equals the configured `FOLLOW_HEADING_DEG` regardless of leader position/course.
    - `COURSE_RELATIVE`: spoof the leader on a known, *changing* course (e.g. a slow turn) with a nonzero `FOLLOW_HEADING_DEG` (e.g. `+90`); confirm the commanded heading tracks `resolveCourseDeg() + FOLLOW_HEADING_DEG` at every sample as the leader turns — i.e. the offset stays constant relative to the direction of travel, not fixed to a compass bearing the way `FIXED` would be. Also confirm it inherits `COURSE`'s low-speed/stationary-fallback behavior (§7.5) since it's built on the same `resolveCourseDeg()` call.
    - **`p1 == 0` edge case:** drive a scenario where the computed heading is exactly 0°/360° (e.g. leader course due north for `COURSE` mode, or a `COURSE_RELATIVE` offset that wraps to exactly 0) and confirm the emitted `p1` is `360`, not `0` — confirm the FC actually updates its heading target that cycle rather than silently skipping it (the firmware-side gotcha documented in §7.7).
    - **Fixed-wing no-op (if a fixed-wing bench unit is available):** confirm a nonzero `p1` has no observable effect on the FW's flight path under normal `GCS NAV`+`NAV POSHOLD` follow conditions, consistent with §7.7's firmware-source finding; note if the OSD's nav-heading indicator shows the commanded value regardless (expected, cosmetic).
    - Confirm `FOLLOW_HEADING_MODE`/`FOLLOW_HEADING_DEG` are live-editable via the web UI (§10.3) the same way every other §9 key already is, and revert to compile-time default on reboot pre-persistence, same as §12.1 item 9.

### 12.2 Flight (progressive, open area, big margins)
1. Trail slot, large horizontal gap, generous vertical separation, low speed. Manual-override switch tested first.
2. Confirm follower captures and holds the slot; confirm instant manual recovery.
3. Confirm peer-lock hold behavior in flight: with two peers airborne, verify the follower locks the first and does not jump to the second if the first's link briefly drops.
4. Only then reduce gaps / try lateral and stacked (overhead) slots.

### 12.3 Acceptance criteria
- Follower reliably captures and holds every configured named slot within a bounded position error, at the configured altitude offset.
- Losing the peer link or flipping the switch always returns control safely (hold → POSHOLD/manual) with no flyaway.
- No `MSP_SET_WP` is ever emitted with stale, out-of-bounds, or mis-scaled coordinates.
- The follower is never commanded to a home-relative altitude below `FOLLOW_MIN_ALT_M`, regardless of the leader's altitude, a descending/landing leader, or the configured vertical offset — the altitude is clamped to the floor, not treated as a reason to withhold the waypoint (§7.6).
- With multiple peers visible, the follower always locks to exactly one (the first detected, or the explicitly configured id) and never auto-retargets to another on loss.
- All follow parameters are editable live via the web UI without a reflash (§10.3); explicitly committed values persist across reboot without a reflash (§10.4). Live edits that are never committed are expected to revert to compile-time defaults on reboot — that is correct behavior, not a bug.
- The follower's commanded nose heading always matches the configured `FOLLOW_HEADING_MODE` (§7.7) — direction of travel, bearing to the leader, or a fixed heading — and `OFF` reproduces pre-§7.7 wire behavior exactly (`p1 == 0` always).

---

## 13. Open Questions / To Confirm

Resolved by source review (kept here for traceability, not action items): PeerManager struct field names/scaling (§5[A]), whether MSPManager has a generic send helper (§6.1, yes — `MSP::command()`), whether MSP_RC/status polling exists (§5[C], partially — `getActiveModes()` reused), scheduler API (§3, none — flat self-gated `loop()`).

Still open:
- **`peer->id` reuse edge case (§6.3):** confirm via bench/multi-peer testing whether a dropped-and-reassigned LoRa slot can plausibly happen within a `LOCKED_HOLDING` window in practice, and whether the name-match mitigation is sufficient or a stronger identity check is needed.
- **Altitude-frame mixing (§6.2, §7.4):** `alt_cm` sums the follower's baro/GPS-fused home-relative estimate with a raw-GPS-only delta from the peer link, because the leader only broadcasts raw GPS telemetry (§1.3 rules out leader-side changes). `FOLLOW_MIN_VSEP_M`'s revised 13 m default is a mitigation (absorb ~8 m of worst-case GPS vertical error), not a fix to the underlying measurement. A future leader-side enhancement — broadcasting the leader's own `MSP_ALTITUDE`-derived home-relative altitude instead of relying on the raw-GPS delta — would remove this error source entirely, but is out of scope for this iteration per §1.3. Worth quantifying with real two-aircraft altitude testing (§12.1 item 8) before deciding whether it's worth pursuing.
- **EEPROM wear from frequent commits (§10.4):** resolved by the two-stage split — live edits (§10.3) never touch EEPROM regardless of how often the panel fires, so this concern is now scoped entirely to the explicit commit action, which is server-side rate-limited/debounced. Still to confirm empirically: whether the chosen rate-limit window is generous enough for normal "tune, then save" pilot workflow without feeling throttled.
- **Whether the `config_init()` `true ||` bug (§10.2) is intentional/known** — confirm with the team before removing it, in case something currently depends on config always resetting (e.g. a support workaround). This only blocks §10.4 (persistence); §10.3 (live editing) can ship and be used independently of this decision.
- Whether an existing FF branch/PR already targets a follow feature or the config-write endpoint independently (check the project's #development channel before building) — a repo-wide grep and recent branch/commit review found no such work in progress as of this spec's writing, but that can change.
- **§7.7 heading control:** whether any real INAV build/config can have `NAV_COURSE_HOLD_MODE` active concurrently with the `GCS NAV`+`NAV POSHOLD` follow gate — if so, the FW controller would read our commanded `p1` as a hard target bearing instead of ignoring it (§7.7's "ignored under normal conditions" finding assumes this doesn't happen). Not expected in practice, but not exercised on real hardware yet — worth a bench check if/when a fixed-wing follower is tested, even though fixed-wing followers remain out of scope (§1.3) for this iteration otherwise.
