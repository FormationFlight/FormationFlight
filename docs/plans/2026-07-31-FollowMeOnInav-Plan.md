# FormationFlight — Autonomous Follow Mode — Implementation Plan

**Spec:** [`docs/spec/2026-07-31-FollowMeOnInav.md`](../spec/2026-07-31-FollowMeOnInav.md)
**Status:** Draft for review

This plan sequences the spec into phases that each produce something bench-
(and eventually flight-) testable, moving from hardcoded/compile-time
behavior to fully runtime-editable web UI config, per the incremental
approach requested. Within each phase, nodes with no edge between them can be
built in parallel (by different people or in parallel work sessions); an
edge `A → B` means B genuinely needs A's code to exist, not just "it would be
nice to sequence it that way."

## Confirmed defaults for Phase 1 (hardcoded, compile-time only)

These came out of the pre-planning discussion and are conservative on
purpose — the goal is a safe first bench/flight test, not an operationally
realistic formation distance:

| Key | Value | Source |
|---|---|---|
| `FOLLOW_SLOT_LONG` / `LAT` / `VERT` | `BEHIND` / `CENTER` / `ABOVE` (chase-high) | user (updated: was `LEVEL`/trail — `ABOVE` chosen so the default is testable with the leader stationary on the ground, not just airborne; see spec §7.3) |
| `FOLLOW_GAP_LONG_M` | 15 m | user |
| `FOLLOW_GAP_LAT_M` | 15 m (unused while slot is chase-high, but must have a sane value since the general resolver reads it) | inferred from "wide margins" |
| `FOLLOW_GAP_VERT_M` | 10 m | user |
| `FOLLOW_MIN_SEP_M` | 8 m | user |
| `FOLLOW_MIN_VSEP_M` | 13 m (5 m physical/collision buffer + 8 m worst-case GPS vertical error budget) | user's original 5 m, revised — see "Unit and altitude-frame correctness" below |
| `FOLLOW_MAX_TARGET_DIST_M` | 50 m | user |
| `FOLLOW_TRIGGER_MODE` | `GCSNAV` | user |
| `FOLLOW_TARGET_PEER` | `FIRST_ACTIVE` | user |
| `FOLLOW_EMIT_HZ` | 4 | spec §8 default |
| `FOLLOW_PEER_TIMEOUT_MS` | 1500 | spec §8 default |
| `FOLLOW_MIN_COURSE_SPEED` | 2 m/s (human-facing; compared internally against `peer->gps.groundSpeed`, which is cm/s — see below) | spec §7.5 default |
| `FOLLOW_STATIONARY_MODE` | `HOLD_COURSE` | spec §7.5 default |
| `FOLLOW_MIN_ALT_M` | 3 m (home-relative absolute floor, clamps `alt_cm` — distinct from `FOLLOW_MIN_VSEP_M`) | spec §7.6 default — found in later review, implemented post-Phase-1, see "Post-Phase 1 addendum" below |

All of §9's keys ship as `#define`s (build_flags) in Phase 1. None are
runtime-mutable or EEPROM-persisted until Phase 3.

### Unit and altitude-frame correctness (found in review, fixed here before Phase 1 code lands)

Neither of these has been coded yet — `FollowManager` doesn't exist in the tree
yet (verified: `grep`-ing the codebase for `FollowManager`/`FOLLOW_MIN_VSEP`/
`FOLLOW_MIN_COURSE_SPEED` turns up nothing outside `docs/`). Only
`MSPManager::sendFollowWaypoint()` (0D) exists today, and it just forwards
whatever `alt_cm` it's given — it doesn't compute it, so there's nothing to
fix there. Both issues below are corrected in the spec (§6.2, §7.4, §7.5, §9)
and must be implemented that way from the start in Phase 1 item 6.

1. **Speed unit mismatch.** `peer->gps.groundSpeed` is `int16_t` **cm/s**
   (spec §5[A]), but `FOLLOW_MIN_COURSE_SPEED`'s stationary-leader threshold
   (spec §7.5) is expressed in m/s. Comparing them directly (`groundSpeed <
   FOLLOW_MIN_COURSE_SPEED`) would compare e.g. `200 < 2`, which never trips
   except when the leader is nearly perfectly still — silently defeating the
   §7.5 stationary/low-speed fallback. Fix: keep the config value in m/s
   (human-facing, matches what the web UI will show) but convert at the
   comparison site: `p->gps.groundSpeed < (int16_t)(FOLLOW_MIN_COURSE_SPEED *
   100)`.

2. **Altitude-frame mismatch widens the real vertical error budget.**
   `alt_cm = local_altitude_cm() + peer->relalt*100 + vert_offset*100`
   (spec §6.2) adds the follower's **baro/GPS-fused home-relative** estimate
   (`MSP_ALTITUDE`) to a **raw-GPS-only** delta (`peer->relalt`, computed in
   `PeerManager.cpp:181` from two `MSP_RAW_GPS` altitudes). This is the best
   available signal given the leader broadcasts only raw GPS telemetry (spec
   §1.3 rules out leader-side changes), but it means the effective vertical
   accuracy is bounded by GPS vertical error (commonly ~8 m per receiver,
   worse than horizontal), not by the FC's more accurate baro-fused estimate.
   Fix: `FOLLOW_MIN_VSEP_M` default raised from 5 m to **13 m** (5 m intended
   physical/collision clearance + 8 m worst-case GPS vertical error budget,
   see table above) so the configured "safety margin" isn't silently consumed
   by altitude-source noise. This is a mitigation, not a fix to the
   measurement itself — see the new Phase 1 test item below and spec §13 for
   the longer-term option (leader broadcasting its own home-relative
   altitude).

### Post-Phase 1 addendum: absolute altitude floor (found in review, after Phase 1 shipped) - [Completed]

Neither the original spec's §7.4 vertical-separation rule nor the shipped
Phase 1 code (`FollowManager.cpp`, `loop()`/`targetSane()`) clamp the
**final** commanded altitude to any absolute minimum. `FOLLOW_MIN_VSEP_M`
only constrains the *configured slot offset* relative to the leader — it
says nothing about what happens if the leader itself descends or lands
while a follower is engaged, or if the slot is configured `BELOW`. In
either case the summed `alt_cm` (spec §6.2: follower's own altitude +
leader's relative altitude + configured vertical offset) can reach zero or
go negative, commanding the follower toward or below its own home
altitude — a flight-into-terrain risk, not a formation-geometry nicety.
This is a gap in the already-shipped Phase 1 code, not just a doc gap —
see spec §7.6 for the full design.

**Fix (small, isolated patch to the existing Phase 1 module) — implemented:**
- Added `FOLLOW_MIN_ALT_M` (default 3 m, home-relative) to
  `src/lib/Follow/FollowConfig.h`, alongside the other `FOLLOW_MIN_*`
  keys (`FollowConfig.h:79-87`).
- In `FollowManager::loop()`, immediately after the altitude sum and
  before `targetSane()` runs, the result is now clamped (not rejected):
  `if (altCm < floorCm) { altCm = floorCm; }`, with `floorCm` computed as
  `lround(FOLLOW_MIN_ALT_M * 100.0)` (`FollowManager.cpp:290-300`, spec
  §6.2/§7.6). Clamping rather than rejecting matters here: rejecting
  would suppress the whole waypoint the way `targetSane()`'s other checks
  do, leaving the follower holding its last position; clamping instead
  lets it keep tracking the leader laterally while holding at a known-safe
  minimum altitude.
- No other Phase 1 decision needed revisiting — purely additive.
- Verified: `pio run -e diy_LoRa_Heltec_WiFi_LoRa_32_433_via_UART` builds
  successfully with the patch in place.

*Test (still outstanding — bench, not yet run):* spec §12.1 item 10 (new)
— spoof the leader descending toward or below the follower's home
altitude with a `BELOW` slot (or a near-zero-relative-altitude leader on a
`LEVEL`/`ABOVE` slot), confirm the commanded `alt_cm` never drops below
`FOLLOW_MIN_ALT_M` and — unlike a `targetSane()` rejection — the waypoint
is still emitted with the floored altitude and correct lat/lon.

This addendum also updates Phase 3/4: `FOLLOW_MIN_ALT_M` is one more §9
key that needs a runtime config field (Phase 3A), a web UI control with
the same non-negative sanity validation client- and server-side as
`FOLLOW_MIN_SEP_M`/`FOLLOW_MIN_VSEP_M` already get (Phase 3B/3C), and
EEPROM persistence (Phase 4B) — no new phase is needed, it just rides
along with the existing Phase 3/4 work for the rest of §9's keys. That
part is still pending Phase 3/4.

---

## DAG overview

```
Phase 0 (foundation, all parallel — no edges between them)
  0A PeerManager: id lookup + peer_is_stale()
  0B MSPManager: MSP_ALTITUDE poll + local_altitude_cm()
  0C MSPManager: GCS-NAV-active accessor (getActiveModes() bit 23)
  0D MSPManager: sendFollowWaypoint() emitter
  0E WiFiManager: extend /peermanager/spoof to accept lat/lon/course params (bench-test tooling)
       │
       ▼
Phase 1 (needs 0A, 0B, 0C, 0D; 0E strongly recommended before 1's bench test)
  1  FollowManager: geometry (§7), PeerLock state machine (§6.3), gate (§6.2/§5C),
     sanity guards (§7.4/§7.5), main.cpp registration — compile-time config only
       │
       ├─────────────────────────────┐
       ▼                              ▼
Phase 2 (needs 1)                Phase 2b (needs 1, independent of 2)
  Read-only status endpoint        AUX-channel trigger mode (optional/stretch —
  GET /followmanager/status        spec's "simple first cut" alt. to GCS-NAV;
  (PeerLock state, locked peer,    not required for MVP since Phase 1 already
  last target) — no config write   ships GCS-NAV trigger per chosen defaults)
       │
       ▼
Phase 3 (needs 2, unblocked — no EEPROM/ConfigHandler involvement)
  3A FollowManager: in-memory runtime config struct (RAM only, seeded from
     Phase 1's compile-time #defines at boot, lost on reboot)
       │
       ▼
  3B WiFiManager: GET/POST /followmanager/config (in-memory only — no EEPROM
     write; validates §7.4 server-side, wires FOLLOW_TARGET_PEER change →
     forced re-acquire per §6.3)
       │
       ▼
  3C html/main.js: new Follow config panel — friendly-grid dropdowns,
     advanced raw-meters toggle, trigger-mode selector, live target-peer
     selector (reads existing /peermanager/status), client-side §7.4
     validation. Fires on every change (cheap — RAM only, no EEPROM wear
     yet); UI marks edits as session-only/"not saved across reboot" since
     there's no persistence until Phase 4.
       │
       ▼
  3D (needs 3C) Nose-heading control (spec §7.7) — rides the same runtime
     config/endpoint/panel 3A-3C already built, adds new fields to each
     rather than new infrastructure. See "Phase 3D" section below.
       │
       ▼
Phase 4 (needs 3D; ALSO gated on a team decision, see "Blocking decision"
  below — this is where the ConfigHandler question actually matters, since
  it's the first phase that touches EEPROM)
  4A ConfigHandler: fix `true ||` bug (prerequisite, isolated change) —
     moved here from the old Phase 3A; only relevant once something is
     actually persisted/reset across reboots
       │
       ▼
  4B FollowManager: EEPROM persistence for 3A's struct, via the same
     primitives config_save()/config_init() use, own EEPROM region after
     cfg's footprint
       │
       ▼
  4C WiFiManager: POST /followmanager/commit — explicit "flush current
     in-memory config to EEPROM" action, separate from 3B's live-edit POST;
     rate-limited/debounced so rapid clicks don't hammer EEPROM
       │
       ▼
  4D html/main.js: "Save permanently" control wired to 4C, distinct from
     3C's live-apply controls; reflects persisted-vs-unsaved state
       │
       ▼
Phase 5 (needs 4)
  5  Full acceptance pass: spec §12.1 bench checklist end-to-end with web UI
     editing + reboot-persistence check (via the explicit commit action),
     then §12.2 progressive flight test
```

### Blocking decision before Phase 4

Spec §13 flags that `ConfigHandler.cpp:37`'s `if (true || cfg.version != VERSION_CONFIG || forcedefault)` may be an intentional "always reset config on boot" behavior that something else currently relies on (e.g. a support workaround), not just a bug. **Confirm with the team before Phase 4A removes it** — this gates all of Phase 4 (EEPROM persistence), but does not block Phases 0–3, which are compile-time, read-only, or in-memory-only and ship independently of this decision. In particular, Phase 3's web UI panel can be built and used for live/session-only tuning before this decision is resolved.

---

## Phase 0 — Foundation (parallel, no dependencies) - [Completed]

Each of these is a small, independent change to existing files. None require the others to exist first, and each is unit-testable / bench-verifiable in isolation before FollowManager consumes them.

- **0A — `src/lib/Peers/PeerManager.{h,cpp}`**: add a lookup-by-id accessor (peer table is index-based today) and `peer_is_stale(peer_t*)` using `peer->updated` vs. `FOLLOW_PEER_TIMEOUT_MS` (spec §8, §11 item 2).
  *Test:* unit-exercise with the existing 5-peer spoof ring; confirm lookup-by-id returns the right record and staleness flips at the expected time.

- **0B — `src/lib/MSP/MSPManager.{h,cpp}`**: add `MSP_ALTITUDE` (109) polling and cache `estimatedActualPosition` as `local_altitude_cm()` (spec §5[B] — confirmed this doesn't exist anywhere today).
  *Test:* log `local_altitude_cm()` on the bench with the FC at a known height; confirm it's home-relative cm, not the GPS/MSL altitude `MSP_RAW_GPS` would give.

- **0C — `src/lib/MSP/MSPManager.{h,cpp}`**: extend the existing `getActiveModes()`-based state read (already used for the ARM bit in `getState()`) with a "GCS NAV active" accessor via `bitRead(activeModes, MSP_MODE_GCSNAV)` (spec §5[C] option 2).
  *Test:* toggle `GCS NAV` on the bench FC, confirm the accessor flips.

- **0D — `src/lib/MSP/MSPManager.{h,cpp}`**: add `sendFollowWaypoint(lat_1e7, lon_1e7, alt_cm)` using the existing `msp_set_wp_t` struct + `MSP::command()` (spec §6.1 — code sample already given, no hand-rolled framing).
  *Test:* call it manually with a known nearby lat/lon, read back with `MSP_WP` (#254) or watch INAV Configurator's WP list; confirm WP#255 lands correctly and `action=1`.

- **0E — `src/lib/WiFi/WiFiManager.cpp`**: extend `/peermanager/spoof` (or add a parametrized sibling) to accept lat/lon/course per request, mirroring `/gnssmanager/spoof`'s existing param handling, instead of only the fixed 5-peer ring (spec §12.1 note). This isn't strictly required to write Phase 1 code, but doing it now means Phase 1's bench tests (verifying each geometry preset lands in the right place) are repeatable rather than constrained to the fixed ring's known positions.
  *Test:* spoof a peer at a controlled lat/lon/course, confirm it shows up correctly in `/peermanager/status`.

---

## Phase 1 — Core follow module (compile-time config only) [Completed]

**Depends on:** 0A, 0B, 0C, 0D (0E recommended for efficient testing, not a hard code dependency).

This is the first flyable slice: hardcoded chase-high slot (behind, centered, above — testable with the leader on the ground), GCS-NAV gate, lock-on-first-peer with hold-not-failover, full geometry math. Nothing is web-editable yet — config values are `#define`s in a new header (e.g. `src/lib/Follow/FollowConfig.h`), seeded with the table above.

Build the general geometry resolver now rather than a special-cased single-preset version, since the grid→canonical expansion (§7.3) is the same amount of code either way and building it twice would be wasted work — the only thing "hardcoded" about Phase 1 is that the `#define` values can't be changed without a reflash.

Work items (single module, best done by one person/session in sequence since they're all one new file, but listed in the order they naturally get written):

1. `src/lib/Follow/FollowManager.{h,cpp}` — new singleton, registered in `main.cpp`'s `loop()` after `PeerManager`/`GNSSManager`/`MSPManager` (spec §3), self-gated at `1000/FOLLOW_EMIT_HZ` ms, skipped until `sys.phase > MODE_OTA_SYNC`.
2. `follow_switch_active()` — gate on the 0C GCS-NAV accessor.
3. `PeerLock` state machine (spec §6.3): `IDLE` / `ACQUIRING` / `LOCKED` / `LOCKED_HOLDING`, lock-by-id via 0A's lookup, name cross-check mitigation for the id-reuse edge case, no auto-failover on loss.
4. Geometry: `slotToLatLon()` (§7.2), `follow_resolve_offset()` expanding the grid config to canonical meters (§7.3).
5. Altitude: `local_altitude_cm()` (0B) + `peer->relalt` + configured vertical offset (§6.2) — comment the frame assumption at the call site (home-relative + raw-GPS delta, see note above and spec §6.2) so it isn't mistaken for an exact conversion.
6. Safety guards: `follow_target_sane()` — min separation, min vertical separation using the revised `FOLLOW_MIN_VSEP_M = 13 m` default, max target distance (§7.4); leader-stationary fallback using `HOLD_COURSE` (§7.5), with `FOLLOW_MIN_COURSE_SPEED` converted to cm/s before comparing against `peer->gps.groundSpeed` (see note above).
7. Emit via 0D's `sendFollowWaypoint()`.

**Test (bench, props off):** spec §12.1 items 1–7, using 0E's parametrized spoof if available:
- Emitter only fires when GCS NAV is active.
- Multi-peer spoof → locks first peer, ignores others, holds (stops emitting) if the locked peer's updates stop, resumes on same id without re-acquire, re-acquires on switch cycle.
- Computed WP#255 for the default chase-high preset lands at the correct position/altitude relative to the spoofed peer and its course; verify the `×10` scaling fix (§5[A]) empirically — this is called out in the spec as the single most safety-critical fact.
- Freshness guard trips within `FOLLOW_PEER_TIMEOUT_MS`.
- Sub-minimum-separation config refuses to arm.
- Stationary-leader unit check: spoof the leader at ~1.5 m/s and ~3 m/s bracketing the 2 m/s `FOLLOW_MIN_COURSE_SPEED` default and confirm the `HOLD_COURSE` fallback triggers/doesn't at the correct threshold — this is the cm/s-vs-m/s comparison from the note above, easy to get silently wrong.
- Altitude accuracy check: with two real GPS units (not spoofed) at a known, measured height difference, confirm the commanded `alt_cm` lands within `FOLLOW_MIN_VSEP_M` of the intended offset — this characterizes the real-world GPS-vertical-error budget the 13 m default is meant to absorb, rather than only bench-testing `local_altitude_cm()` in isolation (0B's test).

**Test (flight, progressive, open area):** spec §12.2 — chase-high slot (behind/center/above), low speed, manual-override tested first, confirm capture/hold and instant manual recovery. Since the leader can be on the ground for this test, first confirm on the bench that the resolved altitude offset is comfortably clear of the leader's rotor wash/airframe before ever arming.

Phase 1 is a legitimate stopping point if flight validation needs to happen before more work lands — everything past this point is additive.

---

## Phase 2 — Read-only status visibility [Complete]

**Depends on:** Phase 1.

- `GET /followmanager/status` in `WiFiManager.cpp` (spec §10.2, the status half only — no config write): current `PeerLock` state, locked peer id/name, gate active/inactive, last computed target. Backed by a `statusJson()` on `FollowManager`.

This needs no EEPROM and no `ConfigHandler` fix, so it's unblocked regardless of the Phase 3 team decision. It mainly exists to make Phase 1's bench tests less dependent on watching the OSD/Configurator, and to de-risk Phase 4's UI panel (which reuses the same status shape for its live view).

*Test:* poll the endpoint during a bench run, cross-check against OSD/Configurator state.

## Phase 2b — AUX-channel trigger (optional / stretch) [Deferred]

**Depends on:** Phase 1 (needs `follow_switch_active()` to exist as a swappable gate).

Not required for the MVP given the chosen Phase 1 default (GCS-NAV trigger), but the spec documents it as the "simple first cut" alternative (§5[C] option 1) and some pilots may prefer decoupling the follow switch from the FC's nav mode during bench testing. Adds `MSP_RC` polling (genuinely new — unused today) + an AUX accessor, and a `FOLLOW_TRIGGER_MODE=AUX` branch in `follow_switch_active()`. Independent of Phases 2/3 — can be picked up whenever, in parallel with either.

---

## Phase 3 — Runtime config (in-memory) + web UI panel [Completed]

**Depends on:** Phase 2 (reuses its status endpoint pattern). Unblocked by the blocking team decision on the `ConfigHandler` reset behavior (see above) — nothing in this phase touches EEPROM. Internally sequential (3A → 3B → 3C) since each step needs the previous to compile/function correctly.

- **3A — `src/lib/Follow/FollowManager.{h,cpp}`**: new runtime config struct covering all §9 keys, separate from `cfg` (don't overload the existing small/unrelated struct). RAM-only: seeded from the Phase 1 compile-time `#define`s at boot, mutated in place by 3B's endpoint, **not** written to EEPROM — a reboot always reverts to the compile-time defaults until Phase 4 lands.
- **3B — `src/lib/WiFi/WiFiManager.cpp`**: `GET /followmanager/config` (resolved values, grid + canonical view) and `POST /followmanager/config` (accepts subset of §9 keys, validates including server-side §7.4 min-separation check, writes only to 3A's in-memory struct, returns resolved config). Changing `FOLLOW_TARGET_PEER` while the gate is active must force `PeerLock` back to `ACQUIRING` (§6.3's explicit escape hatch).
- **3C — `html/main.js`** (or a new component file): new Follow config panel, modeled on the existing `Settings()` component's structure but wired to `/followmanager/config` and `/followmanager/status` (not the dead `/system/status` path `Settings()` currently uses).
  - Friendly-grid dropdowns (§7.3) as the primary editing surface, "advanced" toggle for raw `FOLLOW_OFS_*_M`.
  - Trigger-mode selector (only meaningful if Phase 2b landed; otherwise this is a read-only "GCS NAV" label).
  - Target-peer selector populated live from the existing `/peermanager/status` endpoint (already exists — no new backend work) — this is also the §6.3 "user changes a setting" escape hatch for peer re-acquire.
  - Client-side §7.4 minimum-separation validation that blocks Save (mirrors the server-side check in 3B — both must exist per spec, client-side isn't a substitute for server-side).
  - No debounce needed here — the POST only touches RAM, so firing on every slider drag is cheap. (Debounce comes back in Phase 4, where it actually matters, guarding the EEPROM-write path.)
  - UI clearly communicates that edits are live/session-only and will be lost on reboot until Phase 4's "save permanently" control exists (e.g. a persistent banner/badge, removed once Phase 4 lands).

*Test:* edit each preset from the UI, confirm it takes effect (watch Phase 2's status endpoint or OSD); confirm changing `FOLLOW_TARGET_PEER` mid-flight-mode forces re-acquire; power-cycle the unit and confirm values revert to compile-time defaults (this negative-persistence case is expected/correct in Phase 3 — worth confirming explicitly before Phase 4 adds the positive case).

**Implementation notes (two design points the spec left open, decided during implementation):**
- **`POST /followmanager/config` body format:** form-encoded params (`request->getParam(name, true)`), not a JSON body. Matches every other existing POST endpoint in `WiFiManager.cpp` (`/peermanager/spoof`, `/gnssmanager/spoof`, `/radiomanager/radio_set_enabled`); the codebase has no JSON-body-parsing plumbing (`AsyncCallbackJsonWebHandler` or manual body buffering) and adding one for a single endpoint wasn't justified.
- **Grid vs. raw-offset editing surfaces (§7.3):** `FollowRuntimeConfig` carries an explicit `offsetMode` field (`GRID` | `RAW`, `FollowManager.h`) rather than inferring the active surface from "is any raw offset nonzero" — the latter can't represent a deliberate raw `(0,0,0)` offset unambiguously. Posting any `slotLong`/`slotLat`/`slotVert`/`gapLongM`/`gapLatM`/`gapVertM` param switches the mode to `GRID`; posting any `ofsLongM`/`ofsLatM`/`ofsVertM` switches it to `RAW` (raw wins if both are posted in the same request). `GET /followmanager/config` always reports the resolved canonical `ofsLongM/LatM/VertM` regardless of mode, so the UI's advanced view can show "what this actually resolves to" even while the grid view is active.
- `FOLLOW_TRIGGER_MODE` stays compile-time-only and is reported read-only in `configJson()` (`triggerMode` key) — not accepted by `applyConfig()` — since Phase 2b (AUX trigger) is still deferred and there's nothing for a runtime toggle to switch between yet.
- Server-side and client-side (`html/follow.js`) validation are hand-kept-in-sync mirrors of the same §7.4 geometry rules (3D magnitude, stacked-slot vertical separation) plus basic field sanity (positive `emitHz`/`peerTimeoutMs`/`maxTargetDistM`, non-negative gaps/separations/altitude floor/course speed, in-range `targetPeer`). `FollowManager::applyConfig()` is the source of truth; the client-side copy in `follow.js`'s `validateConfig()` only blocks the Save button early and is not itself trusted.

---

## Phase 3D — Nose-heading control (spec §7.7) [Completed]

**Depends on:** Phase 3C (extends the same `FollowRuntimeConfig` struct, `/followmanager/config` endpoint, and `html/follow.js` panel already built — no new infrastructure). Unblocked by the Phase 4 `ConfigHandler` decision, same reasoning as the rest of Phase 3: this never touches EEPROM.

Adds a `FOLLOW_HEADING_MODE` (default `POINT_LEADER`) + `FOLLOW_HEADING_DEG` pair of keys, live-editable via the web UI like every other §9 key, using WP#255's own `p1` field (spec §7.7) — no second MSP message, no new INAV flight mode, no craft-type branch for fixed-wing followers. Five modes: `OFF`, `COURSE`, `POINT_LEADER`, `FIXED`, and `COURSE_RELATIVE` (added after the initial §7.7 draft — offsets `FOLLOW_HEADING_DEG` from the leader's live course instead of treating it as an absolute compass heading, so a configured "look 90° right of course" angle rotates with the leader's turns; shares the same config field as `FIXED` since only one mode is active at a time).

Work items:
- **`src/lib/Follow/FollowConfig.h`**: add the `FollowHeadingMode` enum (`OFF` / `COURSE` / `POINT_LEADER` / `FIXED` / `COURSE_RELATIVE`) and `#ifndef`-guarded `FOLLOW_HEADING_MODE` (default `FOLLOW_HEADING_POINT_LEADER`) / `FOLLOW_HEADING_DEG` (default `0.0`) defines, following the exact pattern the other `FollowConfig.h` enums/defines already use (e.g. `FollowStationaryMode`/`FOLLOW_STATIONARY_MODE`).
- **`src/lib/Follow/FollowManager.h`**: add `headingMode`/`headingDeg` fields to `FollowRuntimeConfig`, seeded from the new compile-time defines like every other field in that struct already is. Add `int16_t resolveHeadingDeg(const peer_t *peer, double courseDeg) const` to the private interface.
- **`src/lib/Follow/FollowManager.cpp`**:
  - `resolveHeadingDeg()`: switches on `config.headingMode` — `COURSE` reuses the already-computed `courseDeg` (from `resolveCourseDeg()`, called once per `loop()` cycle for the position math, no duplicate work); `POINT_LEADER` calls the existing `GNSSManager::getSingleton()->courseTo(leaderLoc)` (`GNSSManager.cpp:167-171`, already used elsewhere for distance/bearing checks — no new geometry primitive) with `leaderLoc` built from `peer->gps.lat/lon` the same way `targetSane()` already builds `targetLoc`; `FIXED` returns `config.headingDeg` as-is; `COURSE_RELATIVE` returns `courseDeg + config.headingDeg` (same `courseDeg` input as `COURSE`, so it inherits the §7.5 low-speed fallback for free); `OFF` returns `0` directly (the "don't touch heading" sentinel, bypassing the wrap below). All non-`OFF` paths round to `int16_t`, wrap into `[0, 360)`, then map a result of exactly `0` to `360` (spec §7.7's `p1 > 0` gotcha) before returning.
  - `loop()` (`FollowManager.cpp:340`): compute `headingDeg = resolveHeadingDeg(peer, courseDeg)` after `targetSane()` passes, pass it as the new fourth argument to `sendFollowWaypoint()`.
  - `configJson()`/`applyConfig()`: add `headingMode`/`headingDeg` alongside the existing fields — no new validation rule needed beyond "is a recognized enum value" (unlike the geometry fields, there's no unsafe range for a heading in degrees, and `COURSE_RELATIVE`'s offset is a signed value with no bound either).
- **`src/lib/MSP/MSPManager.{h,cpp}`**: extend `sendFollowWaypoint()`'s signature to `(int32_t lat_1e7, int32_t lon_1e7, int32_t alt_cm, int16_t headingDeg)`, setting `wp.p1 = headingDeg` instead of the current hardcoded `wp.p1 = 0` (spec §6.1). Update the function's doc comment to describe `p1`'s new meaning.
- **`src/lib/WiFi/WiFiManager.cpp`**: no new endpoint — `/followmanager/config`'s existing GET/POST handlers already iterate/accept the full `FollowRuntimeConfig`; add the two new fields to the same param list.
- **`html/follow.js`**: add a "Heading" panel (or fold into the existing "Trigger & Target" panel) with a `Setting` dropdown for `headingMode` (`OFF`/`COURSE`/`POINT_LEADER`/`FIXED`/`COURSE_RELATIVE`) and a single `Setting` number field for `headingDeg` shown whenever `headingMode` is `FIXED` **or** `COURSE_RELATIVE` — same conditional-field pattern the panel already uses for the grid-vs-advanced offset toggle (`follow.js:59,167-178`). Swap the field's label text based on which of the two modes is active (e.g. "Heading (absolute °)" vs. "Heading Offset From Course (°)") so the shared field doesn't read as the wrong frame — a UI-copy detail, not a second field. Add `headingMode`/`headingDeg` to `onsave()`'s posted body and to client-side `validateConfig()` (no numeric range needed beyond "is a number"; the 0→360 wrap is a firmware-wire-format concern the server owns, not something the client needs to replicate).

*Test:* spec §12.1 item 11 — verify each mode's commanded heading against a spoofed leader at known position/course (for `COURSE_RELATIVE`, a *turning* leader, to confirm the offset tracks course rather than staying fixed to a compass bearing), the `p1==0`→`360` wrap edge case, and (if a fixed-wing bench unit is available) that the write has no effect on FW flight path. Confirm `FOLLOW_HEADING_MODE`/`FOLLOW_HEADING_DEG` are live-editable via the panel and revert to compile-time default on reboot pre-Phase-4, same as the rest of Phase 3's fields.

**Implemented as designed above**, no deviations:
- `FollowConfig.h`: `FollowHeadingMode` enum + `FOLLOW_HEADING_MODE`(default `FOLLOW_HEADING_POINT_LEADER`)/`FOLLOW_HEADING_DEG`(default `0.0`) defines.
- `FollowManager.h`/`.cpp`: `headingMode`/`headingDeg` added to `FollowRuntimeConfig`; `resolveHeadingDeg()` implements all five modes (`POINT_LEADER` via the existing `GNSSManager::courseTo()`, no new geometry code) and owns the `[1,360]` wrap including the `0`→`360` remap; wired into `loop()` right after `targetSane()` passes; `configJson()` reports both fields.
- `MSPManager.{h,cpp}`: `sendFollowWaypoint()` takes `headingDeg` as a fourth param and writes it to `wp.p1` (only call site is `FollowManager.cpp`, updated).
- `WiFiManager.cpp`: `handleFollowManagerConfigPost()` parses `headingMode`/`headingDeg` the same way as every other §9 key; no new endpoint.
- `html/follow.js`: new "Heading" panel — mode dropdown plus a single degrees field shown for `FIXED`/`COURSE_RELATIVE` with a mode-dependent label; both fields posted in `onsave()`.
- Verified: `pio run -e diy_LoRa_Heltec_WiFi_LoRa_32_433_via_UART` builds successfully.

*Test (still outstanding — bench, not yet run):* spec §12.1 item 11 in full (all five modes, the `p1==0` wrap edge case, and — if a fixed-wing bench unit is available — the no-op confirmation).

---

## Phase 4 — EEPROM persistence [Completed]

**Depends on:** Phase 3D (mutates the same in-memory struct, now including the §7.7 heading fields) **and** the blocking team decision on the `ConfigHandler` reset behavior (see above) — this is the first phase that touches EEPROM. Internally sequential (4A → 4B → 4C → 4D).

- **4A — `src/lib/ConfigHandler.cpp`**: remove the `true ||` short-circuit at line 37, restoring the version-check/force-default logic it was presumably meant to have. Isolated, single-line-scope change; regression-test that normal (non-follow) config still initializes/persists correctly, since this affects the existing `cfg` struct too, not just the new follow config. (Moved here from the old Phase 3A — it only matters once something is actually persisted/reset across reboots, and Phase 3's in-memory-only config doesn't need it.)
- **4B — `src/lib/Follow/FollowManager.{h,cpp}`**: EEPROM persistence for 3A's runtime config struct, via the same EEPROM primitives `config_save()`/`config_init()` use, in its own EEPROM region after `cfg`'s footprint.
- **4C — `src/lib/WiFi/WiFiManager.cpp`**: `POST /followmanager/commit` — an explicit "flush the current in-memory config to EEPROM" action, distinct from 3B's live-edit `POST /followmanager/config`. Rate-limited/debounced server-side so repeated clicks (or an automated caller) don't hammer EEPROM with writes.
- **4D — `html/main.js`**: a "Save permanently" control wired to 4C, visually distinct from 3C's live-apply controls (which only affect the in-memory struct). Reflects persisted-vs-unsaved state — e.g. disabled when the in-memory struct already matches the last-committed EEPROM values, and the "lost on reboot" banner from Phase 3 goes away once a commit succeeds.

*Test:* spec §12.1 item 8 — edit a value via `POST /followmanager/config` (live, in-memory), confirm it takes effect but reverts on reboot without a commit; then call `POST /followmanager/commit`, reboot the follower (or bench unit), confirm the value survived this time (didn't revert to compile-time default). Confirm rapid repeated commits don't cause excessive EEPROM writes (the rate-limit/debounce actually engages).

**Implemented, with deviations from the design above (decided with the user during implementation):**
- **4A was already done.** `ConfigHandler.cpp:37`'s `true ||` short-circuit had already been removed in an earlier commit (`e4c3d44`), before this phase started — verified via `git log -p`. No code change was needed there; `cfg` already round-trips through EEPROM correctly.
- **4D landed in `html/follow.js`, not `html/main.js`.** The Follow config panel lives in `follow.js` (built in Phase 3C), not `main.js`'s unrelated `Settings()` — the plan's file list was stale here.
- **No persisted-vs-unsaved state tracking** (the "disabled when nothing's changed" / banner-removal half of 4D) — descoped by request to keep the button a plain always-clickable action. The "lost on reboot" banner was reworded instead of removed, since Apply-only edits are still session-only.

Design notes not in the original plan:
- `FollowRuntimeConfig`'s EEPROM mirror (`FollowEepromRecord`, `FollowManager.h`) carries its own `FOLLOW_EEPROM_VERSION`, independent of `cfg`'s `VERSION_CONFIG` — a fresh flash or future struct layout change is detected as "nothing saved yet" without touching `cfg`'s versioning.
- `FollowManager::loadFromEEPROM()` reuses `applyConfig()`'s existing §7.4/sanity validation on the loaded record, so a corrupted or stale-schema EEPROM record can't silently arm follow with invalid geometry — it's discarded in favor of compile-time defaults instead.
- The Follow EEPROM region sits at `sizeof(cfg)` (immediately after `cfg`'s own footprint), sized via `sizeof(FollowEepromRecord)`. `ConfigHandler.cpp`'s `config_init()` now calls `EEPROM.begin(sizeof(cfg) + sizeof(FollowEepromRecord))` instead of the old `sizeof(cfg) * 2`, which wasn't big enough to hold the new struct.
- Commit rate-limiting is a flat 2s minimum interval inside `FollowManager::saveToEEPROM()` (`FOLLOW_EEPROM_COMMIT_MIN_INTERVAL_MS`), returning HTTP 429 on the `/followmanager/commit` endpoint when triggered — no separate debounce logic in `WiFiManager.cpp`.
- `html/follow.js`'s "Save to EEPROM" button applies the current form to RAM first (same validation + `POST /followmanager/config` as the existing Apply button) and only calls `POST /followmanager/commit` if that succeeds, matching how a user expects a single "save permanently" action to behave.
- Verified: `pio run -e diy_LoRa_Heltec_WiFi_LoRa_32_433_via_UART` builds successfully with all Phase 4 changes in place.

*Test (still outstanding — bench, not yet run):* spec §12.1 item 8, as originally described above.

---

## Phase 5 — Full acceptance pass

**Depends on:** Phase 4 (exercises the complete system, though most of this is re-running earlier bench/flight tests with the UI as the editing surface instead of hardcoded values/raw `POST` calls).

- Full spec §12.1 bench checklist, §12.2 progressive flight checklist, §12.3 acceptance criteria — this time with all geometry presets reachable via the UI rather than reflashes, and persistence verified through the UI's explicit "Save permanently" (commit) path, not just the live-edit path.
- Revisit spec §13's remaining open question: whether the `peer->id` reuse edge case (dropped-and-reassigned LoRa slot during `LOCKED_HOLDING`) is a real risk in practice, based on what multi-peer bench/flight testing showed in Phases 1 and 5.

---

## Files touched, by phase

| File | Phase(s) |
|---|---|
| `src/lib/Peers/PeerManager.{h,cpp}` | 0A |
| `src/lib/MSP/MSPManager.{h,cpp}` | 0B, 0C, 0D, 2b, 3D |
| `src/lib/WiFi/WiFiManager.cpp` | 0E, 2, 3B, 3D, 4C |
| `src/lib/Follow/FollowConfig.h` (new) | 1, 3D |
| `src/lib/Follow/FollowManager.{h,cpp}` (new) | 1, 2, 3A, 3D, 4B |
| `src/main.cpp` | 1 |
| `src/lib/ConfigHandler.cpp` | 4A |
| `html/main.js` | 3C, 4D |
| `html/follow.js` (new) | 3C, 3D |
| `targets/*.ini` | 1 (seed `build_flags`), 3D (seed §7.7 keys) |

## Open items carried from the spec (not yet resolved by this plan)

- Confirm the `ConfigHandler` `true ||` reset is unintentional before Phase 4A (blocking decision, above).
- `peer->id` reuse edge case — deferred to real multi-peer testing in Phases 1/5, per spec §13.
- **Altitude floor (`FOLLOW_MIN_ALT_M`, spec §7.6) is now implemented** (see "Post-Phase 1 addendum" above) — bench test item §12.1 #10 (spoofed low/descending leader) is still outstanding, and the runtime-config/web-UI half rides along with Phase 3/4.
- **Nose-heading control (`FOLLOW_HEADING_MODE`, spec §7.7) is planned but not yet implemented** — see "Phase 3D" above. Spec §13's open question on whether `NAV_COURSE_HOLD_MODE` can coincide with `GCS NAV` follow on a fixed-wing FC (which would make the FW controller actually consume the commanded `p1` instead of ignoring it) is unverified on real hardware; worth a bench check if/when a fixed-wing follower is ever tested, though fixed-wing followers remain out of scope (§1.3) otherwise.

## Run the test Server to help test the UI
`python3 .claude/skills/web-ui-preview/mock_server.py`