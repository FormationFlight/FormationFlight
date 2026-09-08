# FormationFlight — Follow Module Test Suite — Engineering Spec

**Status:** Draft — not yet planned or implemented
**Target:** `src/lib/Follow/` (C++: `FollowManager.{h,cpp}`, `FollowConfig.h`) and `html/follow.js`
**Depends on / asserts against:**
- [`2026-07-31-FollowMeOnInav.md`](2026-07-31-FollowMeOnInav.md) — canonical offset model (§7.3), geometry math (§7.2), safety rules (§7.4), altitude floor (§7.6), heading modes (§7.7), peer lock state machine (§6.3). Its own §12 ("Test & Acceptance") is effectively a manual bench/flight test plan already — several items there map directly onto automated cases below.
- [`2026-08-13-FollowStatusOsdGvar.md`](2026-08-13-FollowStatusOsdGvar.md) — status/condition GVAR contract, change+heartbeat send rule (§3.3).
- [`2026-08-15-FollowRcAxisControl.md`](2026-08-15-FollowRcAxisControl.md) — RC-to-offset mapping (§3), two-layer geometry safety net (§4), pre-arm check (§4.6).
- [`2026-08-28-FollowSpeedAutothrottle.md`](2026-08-28-FollowSpeedAutothrottle.md) — kinematic braking law (§4), three-way engage gate (§3.6).
- `docs/user-guide-follow-mode.md` — pilot-facing behavioral contract (lock states, GVAR value tables, RC freeze/pre-arm behavior, REST API field reference). Written *from* the specs above; treated here as a secondary, cross-checking source, not a third independent truth.

---

## 1. Purpose & Scope

### 1.1 Problem

`src/lib/Follow/` has grown across five feature passes (base follow-me, OSD/GVAR status, RC axis trim, speed autothrottle, and assorted fixes) into a 1154-line `FollowManager.cpp` with no automated tests anywhere in the repo. A cleanup refactor of this module is wanted, but with zero test coverage there is currently no way to tell "I made the code cleaner" apart from "I silently changed what altitude floor clamping does." This spec designs the test suite that needs to exist *before* that refactor starts, so the refactor has something to run against.

### 1.2 Goal

A test suite that:
1. Runs on the developer's machine and in CI, with no ESP32/flight-controller hardware attached.
2. Exercises `FollowManager`'s actual decision logic (state machine, geometry/safety math, GVAR reporting, config validation) — not just the handful of already-free-standing pure functions.
3. Encodes the behavioral contract from the four specs above (and cross-checks against the user guide), so it catches both accidental regressions *and* pre-existing spec/code mismatches, surfacing the latter for review rather than encoding them as "correct."
4. Extends to `html/follow.js`'s client-side logic, since it duplicates (deliberately, in part) the same validation/geometry rules the firmware enforces, and a refactor is just as likely to desync those as it is to break the firmware alone.

### 1.3 In scope

- `src/lib/Follow/FollowManager.{h,cpp}`, `FollowConfig.h` — the full public interface (`loop()`, `applyConfig()`, `statusJson()`, `configJson()`, `loadFromEEPROM()`/`saveToEEPROM()`) and the private decision logic behind it.
- `html/follow.js`'s pure logic: `slotFromOffset()`/`offsetFromSlot()` (friendly-grid ⇄ raw offset conversion) and `validateConfig()` (client-side config validation).
- The three-way config-validation relationship between `FollowManager::applyConfig()` (C++, authoritative), `.claude/skills/web-ui-preview/mock_server.py`'s `validate_config()` (Python, a deliberate mirror of the C++ rules only), and `html/follow.js`'s `validateConfig()` (JS, a strict superset — see §2.3).
- The minimal dependency-injection seams needed inside `FollowManager` to make the above testable off-hardware (§3.2) — this is enabling groundwork for the test suite, not the cleanup refactor itself, and should land as its own reviewed change before the cleanup starts.

### 1.4 Out of scope (this iteration)

- The cleanup/refactor of `Follow` code itself — this spec only covers what protects that future work.
- Core FormationFlight code outside `Follow`: `PeerManager`, `MSPManager`, `GNSSManager`, `WiFiManager`, radio/OTA sync, etc. are treated as dependencies to fake, not targets to test here.
- Real GPS/compass/RF behavior, actual flight dynamics, and MSP wire-protocol byte-level correctness — these stay in the existing bench/HITL/SITL testing docs (`docs/explainers/bench-testing-follow-mode.md`, `hitl-testing-follow-mode.md`, `sitl-testing-follow-mode.md`), not in this automated suite (§5).
- Visual/DOM testing of the Follow web panel (`html/main.js` rendering, click-through flows) — only `follow.js`'s extracted pure logic is in scope, not full component/UI testing.

### 1.5 Ground rules (decided up front, binding for this spec)

- **Seams over hardware-in-the-loop.** Testability groundwork (interfaces + fakes for `MSPManager`/`GNSSManager`/`PeerManager`, §3.2) is worth doing now, so the suite can run headless in CI on every commit rather than needing real/emulated hardware.
- **Spec is truth, but code is never silently changed to match it.** Where a test built from `docs/spec/*.md` or the user guide reveals current code disagrees with the documented behavior, the test is written to assert the *spec's* behavior, marked as **currently failing**, and reported as a finding — the fix is a separate, reviewed change, not something this test-writing effort does on its own. (§7 tracks findings surfaced while drafting this spec.)
- **Cross-mirror consistency is a first-class test target**, not an afterthought — `applyConfig()`/`validate_config()`/`validateConfig()` and `configJson()`/mock server `DEFAULT_CONFIG` are three independent, hand-maintained surfaces already flagged in this repo's `CLAUDE.md` as a drift risk.
- **`follow.js` is in scope**, tested independently in a lightweight Node harness (§3.5) — not full browser/DOM testing, just its extracted pure functions.

---

## 2. Current State (verified against source)

### 2.1 No existing test infrastructure

There is no `test/` directory, no PlatformIO `native` environment in `platformio.ini` or `targets/*.ini` (every `[env:...]` there targets `esp32`/`esp32s2`/`esp8266`), and no mocking pattern for any manager anywhere in the codebase. The closest things to a test harness today are the web-UI mock server (`.claude/skills/web-ui-preview/mock_server.py`) and the runtime `/peermanager/spoof` bench-testing endpoint — both manual/integration aids, not automated tests.

### 2.2 `FollowManager` reaches directly into three hardware singletons

`FollowManager.cpp` calls `MSPManager::getSingleton()`, `GNSSManager::getSingleton()`, and `PeerManager::getSingleton()` directly, inline, throughout — there is no injection point today. The full I/O surface actually used:

| Manager | Methods called from Follow | Used for |
|---|---|---|
| `MSPManager` | `getState()`, `isGCSNavActive()`, `getRcChannelUs()`, `local_altitude_cm()`, `getPlatformType()`, `sendFollowWaypoint()`, `sendSetHead()`, `isHeadingHoldActive()`, `sendGvar()` | Follow gate, RC reads, own altitude, airframe type, all outbound MSP writes |
| `GNSSManager` | `getSingleton()->horizontalDistanceTo()`, `courseTo()`, static `calculatePointAtDistance()` | Self-position-relative distance/bearing math, POINT_LEADER heading, along-track error |
| `PeerManager` | `getPeerById()`, `getPeer()`, free function `peer_is_stale()` | Peer table lookups in `resolveLock()` |

Everything except `slotToLatLon()` (FollowManager.cpp:35-67) and four file-local `static` functions — `offsetGeometrySane()`, `axisSignLocked()`, `candidateOffsetOk()`, `rcCandidateMatchesStaticDefault()` (FollowManager.cpp:302-409) — is a `FollowManager` method that reaches into one of these singletons. Those five functions are genuinely pure today and need no seam at all (§4.14 tests them directly, right now, with zero groundwork).

### 2.3 Discovered: three config validators, not two — and they deliberately disagree

While tracing `applyConfig()` for this spec, a third validator turned up: `html/follow.js`'s `validateConfig()` (follow.js:42-81) is not just a mirror of `FollowManager::applyConfig()` (FollowManager.cpp:909-1020) — it's a **strict superset**. It additionally enforces:
- GVAR index uniqueness across all four GVAR fields (`statusGvarIndex`, `conditionFlagsGvarIndex`, `targetSpeedGvarIndex`, `autothrottleEngageGvarIndex`),
- RC channel uniqueness across the three axis channels,
- `autothrottleEnableRcChannel` distinct from the axis channels,
- `autothrottleEnableMaxThresholdUs > autothrottleEnableMinThresholdUs`.

None of these four rules exist in `FollowManager::applyConfig()`. `mock_server.py`'s `validate_config()` (mock_server.py:272-308) correctly mirrors *only* the C++ rules and has an explicit comment noting the threshold-ordering check is "UI-only... deliberately not duplicated here." This looks intentional (defense-in-depth in the UI, not a duplicated guard), matching the user guide's own §3/§7.2 wording ("the UI blocks...") — but it does mean a raw `POST /followmanager/config` (bypassing the web UI entirely, e.g. via `curl` per §5's spoofing workflow) can currently set overlapping GVAR indices or RC channels on the real firmware. This is a **finding for review, not a bug this spec fixes** — see §7.1.

---

## 3. Test Suite Architecture

### 3.1 A new PlatformIO `native` environment

Add an `[env:test_native]` (or similar) to `platformio.ini`, using PlatformIO's `native` platform — compiles and runs on the host (Linux/Mac/CI runner), no board, no upload step. This env only needs to build `src/lib/Follow/*` plus its test doubles and the small slice of `main.h`/`PeerManager.h` structs it depends on for types (`peer_t`, `cfg`/`sys`) — it does **not** need to build `main.cpp` or any board-specific manager implementation. This is additive: it changes nothing about the existing `targets/*.ini` board environments or the CI matrix that builds them (`.github/workflows/build.yml`), it's a new, separate CI job.

PlatformIO's `native` platform bundles Unity as its default test framework — use it as-is rather than pulling in Catch2/GoogleTest, since it needs zero extra dependency wrangling and the project already has no opinion pulling the other way.

### 3.2 Seams: three thin interfaces, not a rewrite

The minimum-invasive change is to introduce one interface per external manager Follow touches, with `FollowManager` holding a pointer to each (defaulted to the real singleton in production, overridable in tests):

```cpp
// New, e.g. src/lib/Follow/FollowDeps.h
class IFollowMsp {
public:
    virtual uint8_t getState() = 0;
    virtual bool isGCSNavActive() = 0;
    virtual bool getRcChannelUs(uint8_t channel1Based, uint16_t *outUs) = 0;
    virtual int32_t local_altitude_cm() = 0;
    virtual InavPlatformType getPlatformType() = 0;
    virtual void sendFollowWaypoint(int32_t lat_1e7, int32_t lon_1e7, int32_t alt_cm, int16_t headingDeg) = 0;
    virtual void sendSetHead(int16_t headingDeg) = 0;
    virtual bool isHeadingHoldActive() = 0;
    virtual void sendGvar(uint8_t index, int32_t value) = 0;
};

class IFollowGnss {
public:
    virtual double horizontalDistanceTo(GNSSLocation b) = 0;
    virtual int16_t courseTo(GNSSLocation b) = 0;
    // calculatePointAtDistance() stays a free/static pure function — no seam needed (§2.3/§4.2).
};

class IFollowPeers {
public:
    virtual const peer_t *getPeerById(uint8_t id) = 0;
    virtual const peer_t *getPeer(uint8_t index) = 0;
};
```

Production code wires the real singletons through thin adapters (`MSPManager::getSingleton()` already exposes every method `IFollowMsp` needs — the adapter is a forwarding shim, not new logic). `FollowManager` gains a constructor/setter that defaults to these real adapters, so `main.cpp` and `getSingleton()` need a one-line change, nothing else in the app changes behavior.

This is intentionally the *only* structural change this spec asks for ahead of the cleanup refactor — everything else (extracting more pure functions, reorganizing `loop()`, etc.) is left to that refactor, informed by the coverage this suite then provides.

### 3.3 Fakes

One fake per interface, living under the test env only:

- **`FakeMsp : IFollowMsp`** — settable `armed`/`gcsNavActive`/`platformType`/`altitudeCm`/`headingHoldActive`, a settable-per-channel µs map for `getRcChannelUs()` (including "channel not populated" simulation), and recorders for every outbound call (`sentWaypoints`, `sentHeadings`, `sentGvars` — vectors of call args) so tests can assert both *that* something was sent and *what*.
- **`FakeGnss : IFollowGnss`** — a settable "self" `GNSSLocation`; `horizontalDistanceTo()`/`courseTo()` implemented against it using the same great-circle math `GNSSManager` uses today (or, more robust: literally delegate to `GNSSManager`'s real static helpers, since those are pure and already trustworthy — only the *self-position* lookup is what needs faking, not the math).
- **`FakePeers : IFollowPeers`** — a settable peer table (`std::vector<peer_t>` or fixed array matching `NODES_MAX`), with helpers like `setPeer(id, lat, lon, groundSpeed, groundCourse, relalt, lastUpdateMs)` and `markStale(id)` for driving the freshness (`peer_is_stale()`) checks `resolveLock()` depends on.

### 3.4 `main.h` globals (`cfg`, `sys`)

`loop()`'s very first check is `sys.phase <= MODE_OTA_SYNC` (FollowManager.cpp:494) and `FOLLOW_EEPROM_OFFSET` depends on `sizeof(cfg)`. The native test env links a minimal stand-in `main.h`/`main.cpp` translation unit providing just `cfg`/`sys` as plain structs with test-controllable fields — not the real `main.cpp`, which pulls in the full radio/OTA/WiFi stack. `sys.phase` needs to be settable per test so state-machine tests aren't gated behind a simulated OTA sync every time.

### 3.5 `follow.js`: a Node-based harness, no browser

`html/` has no build step — `follow.js` is loaded directly as an ES module by the browser. Its pure logic (`slotFromOffset`, `offsetFromSlot`, `validateConfig`, lines 25-81) doesn't touch the DOM or Preact at all; only `FollowPanel()` (line 114 on) does. Two options, in order of preference:

1. **Extract the pure functions into their own module** (e.g. `html/follow-logic.js`), imported by both `follow.js` (for the UI) and a new `test/follow-logic.test.js`, run with Node's built-in `node:test` + `node --test` — zero new npm dependency, zero build step added, matches the "no build step" philosophy `html/` already has.
2. If extraction is judged too invasive for this phase, fall back to importing `follow.js` directly in Node and stubbing `./bundle.js`/`./components.js` (both only used by `FollowPanel()`, not by the functions under test) with empty modules — noisier, but avoids moving code before the refactor.

Recommend option 1: it's a small, mechanical, low-risk move (pure functions into their own file) that also happens to be a preview of the kind of extraction the C++ refactor will want to do — cheap to validate the pattern here first.

### 3.6 The cross-mirror fixture

A single data file — e.g. `docs/spec/fixtures/follow-config-cases.json` — of `{name, config, expectValid, expectedErrorSubstring?}` entries, covering every rule in §2.3's table (the shared C++/Python rules) plus a separately-marked set covering the JS-only superset rules. Three thin test files consume the same fixture:

- C++ native test: constructs a `FollowRuntimeConfig` from each case, calls `applyConfig()`, asserts pass/fail matches `expectValid` (shared cases only).
- Python test (new, e.g. `.claude/skills/web-ui-preview/test_mock_server.py`, or wherever the project prefers Python tests to live): calls `validate_config()` per case, same assertion (shared cases only).
- JS/Node test: calls `validateConfig()` per case, asserting the shared cases *and* the JS-only superset cases.

This makes the §2.3 divergence explicit and enforced rather than incidental: the fixture format itself documents which rules are "must match across all three" vs. "JS-only, by design," so a future change to one validator without updating the fixture (and thus the others) fails a test instead of silently drifting further.

---

## 4. Coverage Plan

Each item below is a group of test cases, cited against the spec section it encodes. "Seam-free" items need none of §3's groundwork and can be written first, immediately, as a way to get the native env stood up against something real before tackling the state-machine/loop() tests.

### 4.1 Peer lock state machine (parent spec §6.3, `resolveLock()`, FollowManager.cpp:95-171) — needs `FakePeers`
- Fresh `FollowManager`: gate active + one live peer → `IDLE → ACQUIRING → LOCKED` within one `loop()` cycle once a candidate exists; stays `ACQUIRING` with no peers.
- `targetPeer == 0` ("First Active"): locks the first peer with `id > 0` and not stale, in `PeerManager` iteration order.
- `targetPeer` pinned to a specific id: only locks that id, ignores other live peers.
- Locked peer goes stale → `LOCKED_HOLDING`, `resolveLock()` returns `nullptr`, no new target emitted, but same `lockedId` retained.
- Locked-holding peer's telemetry returns *with the same name* → returns to `LOCKED`, same id.
- Locked-holding peer's telemetry returns *with a different name at the same id* → id-reuse-mismatch path: `lockedId` reset to 0, stays effectively acquiring-but-not-relocking (per spec §6.3 "only a gate cycle can recover") — verify it does **not** silently pick up the new aircraft under the old id.
- Gate goes inactive mid-lock → forces `IDLE`, clears `lockedId`/`lockedName`/`haveValidCourse` (FollowManager.cpp:532-541).
- `applyConfig()` changing `targetPeer` while locked → `forceReacquire()` fires (`state=ACQUIRING`, `lockedId=0`) even mid-flight, per spec §6.3's explicit escape hatch.

### 4.2 Slot geometry math (parent spec §7.1/§7.2, `slotToLatLon()`) — seam-free, pure
- The four cardinal single-axis offsets (pure Ahead, pure Behind, pure Left, pure Right) at `course_deg = 0` (due-north leader) — verify resulting bearing/distance match hand-computed expected values.
- A combined offset (e.g. the "chase-high" default: Behind 15m + Above 10m — note vertical isn't part of `slotToLatLon()`'s lat/lon math at all, so this is really "Behind 15m" alone at the lat/lon layer) at a non-zero `course_deg` (e.g. 90°, 270°) — verifies the rotation math, not just the axis-aligned cases.
- `course_deg` wraparound near 0°/360° boundary.
- Confirm the projected point round-trips sanely against `GNSSManager::calculatePointAtDistance()`'s own contract (same bearing/distance in, same lat/lon out) — this is testing `slotToLatLon()`'s bearing/distance derivation, not re-testing `calculatePointAtDistance()` itself.

### 4.3 Safety bounds — geometry sanity (parent spec §7.4, `offsetGeometrySane()`) — seam-free, pure
- 3D magnitude exactly at `minSepM` → passes (boundary is inclusive per `<` not `<=` at FollowManager.cpp:309).
- 3D magnitude just under `minSepM` → fails, with the documented error substring.
- Non-stacked slot (horizontal magnitude ≥ `FOLLOW_STACKED_HORIZONTAL_EPSILON_M`) with a small vertical offset → passes regardless of `minVSepM` (the stacked rule shouldn't apply).
- Stacked slot (near-zero horizontal) with vertical offset under `minVSepM` → fails; at/above `minVSepM` → passes.
- `applyConfig()` rejecting a bad static config end-to-end (not just the pure helper) — confirms the caller wiring, not just the math.

### 4.4 Altitude floor (parent spec §7.6) — needs `FakeMsp` (own altitude) + `FakePeers` (relalt)
- Computed altitude above the floor → not clamped, `floorClamped == false`, no condition code raised.
- Computed altitude below the floor → clamped exactly to `floorCm`, waypoint still emitted (clamp, not suppress — this is the single most important "don't regress" invariant per the parent spec's explicit framing).
- Clamp attribution: a floor breach that *would also* occur with the static (non-RC-scaled) vertical offset → `FOLLOW_CONDITION_FLOOR_CLAMPED`; a floor breach caused specifically by RC pushing the vertical axis further down than the static default → `FOLLOW_CONDITION_RC_INVALID_GAP_SETTINGS` takes priority (FollowManager.cpp:581-607) — needs both an RC seam and altitude seam together.

### 4.5 Heading modes (parent spec §7.7, `resolveHeadingDeg()`) — mostly seam-free; `POINT_LEADER` needs `FakeGnss`
- `OFF` → returns `0` unconditionally (the wire sentinel) regardless of course/config.
- `COURSE` → returns `courseDeg` as passed in, wrapped into `[1, 359]`.
- `FIXED` → returns `config.headingDeg`, independent of `courseDeg`/peer.
- `COURSE_RELATIVE` → `courseDeg + headingDeg`, wrapped.
- `POINT_LEADER` → delegates to `GnssFake::courseTo(leaderLoc)` built from `peer->gps.lat/lon`.
- The `0/360 → 1` sentinel-collision remap (FollowManager.cpp:242-255): a config that would compute exactly `0` or `360` returns `1`, not `0` (which would be misread downstream as "don't send a heading").
- `loop()`-level: `headingDeg == 0` (OFF) skips calling `sendSetHead()` entirely, even when `isHeadingHoldActive()` is true; a nonzero heading with `isHeadingHoldActive() == false` also skips the send (FollowManager.cpp:659-662) — both gates need independent coverage, not just "at least one heading gets sent somewhere."

### 4.6 Course fallback (parent spec §7.5, `resolveCourseDeg()`) — needs `FakePeers`
- Peer ground speed above `minCourseSpeed` → uses live `groundCourse`, updates `lastValidCourseDeg`/`haveValidCourse`.
- Peer ground speed drops below threshold *after* a valid course was captured → holds the last valid course, does not adopt the new (jittery) reading.
- Peer ground speed below threshold from the very first cycle (no valid course ever captured) → falls back to whatever's reported rather than an arbitrary 0 (explicitly documented fallback-of-last-resort, FollowManager.cpp:201-204).

### 4.7 RC axis mapping (RcAxisControl spec §3, `resolveAxisOffset()`) — needs `FakeMsp`
- No channel assigned (`channel1Based < 1`) → returns `configuredM` unchanged, no MSP call.
- Channel assigned, MSP read fails (`getRcChannelUs()` returns false — no FC / out-of-range channel) → falls back to `configuredM`.
- `us = 1500` → `0`; `us = 2000` → `+gap`; `us = 1000` → `-gap`; values outside `[1000,2000]` clamp to the nearest endpoint before mapping (not rejected).
- `gap = |configuredM|` — confirm a negative configured default (e.g. "Behind 15m" = `-15`) still produces `gap = 15`, `+gap` at `us=2000`.

### 4.8 RC two-layer safety net (RcAxisControl spec §4, `axisSignLocked()`/`candidateOffsetOk()`) — seam-free, pure
- Layer 1 (geometry): a candidate that fails `offsetGeometrySane()` on its own is rejected regardless of Layer 2.
- Layer 2 (sign-lock): candidate's axis flips sign relative to `lastKnownGood` while the *other two* axes' combined magnitude is below `minSepM` → rejected (the "would pass directly through the leader" case).
- Same sign flip, but the other two axes' combined magnitude is *above* `minSepM` → allowed (there's a safe path around, not through).
- A zero-crossing (either side is exactly `0`, not a genuine sign flip) → never counts as "crossed" (FollowManager.cpp:336-341) — boundary itself isn't a side.
- `resolveOffset()` end-to-end: a rejected candidate leaves `lastKnownGood` (and thus the emitted offset) exactly unchanged from the prior cycle — the freeze behavior itself, not just the pure predicate.
- `applyConfig()` resets `lastKnownGood` to the new static offset on every successful apply (parent RcAxisControl spec §4.4's reset rule) — verify a config change while RC-frozen doesn't leave a stale frozen value from the *old* config in place.

### 4.9 Pre-arm check (RcAxisControl spec §4.6) — needs `FakeMsp` (armed state + RC)
- Disarmed + at least one RC axis assigned → `havePreArmCandidateOffset == true`, computed every cycle.
- Armed (regardless of RC assignment) → `rcPreArmCheckFailed` forced to `false`, `havePreArmCandidateOffset == false`, every cycle (never sticky/stale from a prior disarmed cycle).
- Disarmed, RC-assigned axis at center (0 offset) with a nonzero configured default on that axis → fails the check (center ≠ static default is the documented "don't assume center is safe" case, user guide §7.4).
- Disarmed, RC-assigned axis pushed to the exact sign/magnitude of the static default (within `FOLLOW_PREARM_MATCH_EPSILON_M`) → passes.
- Confirms this check never mutates `lastKnownGood` (read-only simulation, per the code comment at FollowManager.cpp:517-520) — assert `lastKnownGood` unchanged after a pre-arm-check-triggering cycle.

### 4.10 Speed autothrottle (SpeedAutothrottle spec §3/§4) — needs `FakeMsp` (platform type, arm channel) + `FakePeers`
- Three-way engage gate, tested independently for each leg: not locked → `engaged=false`; locked but platform ≠ airplane → `false`; locked + airplane but arm channel outside its armed range → `false`; all three true → `true`. No latching — flip any one false mid-session and `engaged` drops immediately next cycle.
- `resolveTargetSpeedCmS()`'s kinematic braking law: `speedCorrectionAccelCmS2 == 0` → pure feedforward, target speed exactly equals `peer->gps.groundSpeed` regardless of along-track error.
- Nonzero accel, follower lagging behind its slot (positive along-track error) → correction adds to leader's speed; follower ahead (negative error) → correction subtracts. Verify the `sqrt(2*a*d)` magnitude against a hand-computed value for at least one nontrivial `(a, d)` pair.
- Clamping: a computed speed above `maxTargetSpeedMps`/below `minTargetSpeedMps` is clamped, not passed through raw.
- `autothrottleArmed()`: unassigned arm channel (`< 1`) → always armed; assigned, MSP read fails → armed (same no-FC fallback as `resolveAxisOffset()`); assigned and readable → armed iff pulse width falls within `[min, max]` inclusive.

### 4.11 Status/condition GVAR reporting (OsdGvar spec §3.3, `updateStatusGvars()`/`updateAutothrottleGvars()`) — needs `FakeMsp` recorder
- Value changes between cycles → sent immediately.
- Value unchanged, less than `FOLLOW_GVAR_HEARTBEAT_MS` since last send → not resent (verify via the fake's call recorder — zero calls, not just "same value again").
- Value unchanged, `FOLLOW_GVAR_HEARTBEAT_MS` elapsed → resent as a heartbeat.
- First-ever cycle (sentinel `INT32_MIN`) → always sends, establishing the spec's "write 0 explicitly at startup" behavior even when the computed value happens to be 0.
- Condition code priority (`raiseCondition()`, FollowManager.cpp:600-612): when floor-clamp, target-too-far, and RC-freeze conditions are simultaneously true in one cycle, the *highest-valued* code wins, not the first one computed — construct a case that triggers multiple conditions in one cycle and assert only the highest is reported.
- GVAR index `-1` (disabled) → never calls `sendGvar()` for that index, zero MSP traffic, regardless of state changes.

### 4.12 `targetTooFar()` and the geometry-sane early-return (parent spec §7.4 runtime check) — needs `FakeGnss`
- Target within `maxTargetDistM` of follower's own position → waypoint emitted normally.
- Target beyond `maxTargetDistM` → waypoint suppressed entirely (no `sendFollowWaypoint()` call), `FOLLOW_CONDITION_TARGET_TOO_FAR` raised, but state/lock machinery is untouched (still `LOCKED`, not dropped).
- A resolved offset that fails `offsetGeometrySane()` at emit time (not just at `applyConfig()` time — e.g. RC having frozen to something that later fails once `minSepM` is live-edited smaller mid-flight) → also suppresses the waypoint, no condition code double-counted against `TARGET_TOO_FAR`'s.

### 4.13 Config validation (`applyConfig()`, FollowManager.cpp:909-1020) and EEPROM round-trip
- Every individual rejection rule in the function (§2.3's table) — one case each, both the boundary-fails and boundary-passes value.
- A config that fails validation leaves `config` (the live, active one) completely untouched — verify by reading `getConfig()` before/after a rejected `applyConfig()` call.
- `toEepromRecord()`/`fromEepromRecord()` round-trip: every field survives a write-then-read unchanged (within `int16_t` truncation for the geometry/speed doubles — verify the *documented* precision loss is the only loss, e.g. `15.4` truncating to `15`, not something worse).
- `loadFromEEPROM()` with a version mismatch (stale/uninitialized EEPROM) → falls back to compile-time defaults untouched, does not crash or partially apply a corrupt record.
- `saveToEEPROM()` rate limiting: two calls within `FOLLOW_EEPROM_COMMIT_MIN_INTERVAL_MS` → second one fails with an error message, first one's data is what persists.

### 4.14 REST contract shape (`configJson()`/`statusJson()`) — golden-field test
- `configJson()` emits every field the user guide's §11 table documents, with matching key names — a schema/field-presence test, not a value test. This is what stops a refactor from silently renaming/dropping a field the web UI or an external `curl`-scripting user (§5 workflow) depends on.
- `statusJson()`'s conditionally-present fields (`lastTarget`, `liveOffset`, `preArmCandidateOffset`, GVAR-value fields) appear/disappear under exactly the documented conditions (`haveLastTarget`, `havePreArmCandidateOffset`, GVAR index `>= 0`) — one case per conditional field, both present and absent.

### 4.15 Cross-mirror equivalence (§3.6's fixture) — C++, Python, and JS
- Every shared-rule fixture case produces the same valid/invalid verdict from `applyConfig()`, `mock_server.py`'s `validate_config()`, and (for the shared subset only) `follow.js`'s `validateConfig()`.
- The JS-only superset cases (§2.3) are asserted against `follow.js` alone, with an explicit comment/marker in the fixture noting they're intentionally not required to match the other two.

### 4.16 `follow.js` pure functions (§3.5)
- `slotFromOffset()`/`offsetFromSlot()` round-trip for all three axes' label pairs, including the `0 → CENTER/LEVEL` case.
- `validateConfig()`'s own rule set — both the shared subset (covered by §4.15's fixture) and the JS-only rules (GVAR/RC/autothrottle-channel uniqueness, threshold ordering) with dedicated cases.

---

## 5. What stays out of automated testing (and why)

- **Literal MSP wire timing/byte encoding** (e.g. `nextRunTime`'s `emitHz`-derived rate limiting actually producing N messages/second on a real serial link, `EEPROM.commit()`'s actual flash-write behavior) — these are hardware/timing properties, not decision logic; the existing bench/HITL docs already cover verifying them on real hardware.
- **Real GPS/compass accuracy, actual flight dynamics, RF link behavior** — SITL/HITL territory (`docs/explainers/sitl-testing-follow-mode.md`, `hitl-testing-follow-mode.md`), unrelated to whether `FollowManager`'s decision logic is correct given some input.
- **Full `FollowPanel()` component/DOM rendering** — only its extracted pure logic (§3.5/§4.16) is covered; testing Preact rendering/click-through would need a much heavier browser-test setup for comparatively little regression protection, given the panel itself is a thin view over `configJson()`/`statusJson()`.

---

## 6. Rollout Plan

1. **Phase 1 — enabling groundwork (reviewed separately from the cleanup refactor):** add the `native` PlatformIO env (§3.1), the three interfaces + production adapters (§3.2), and the fakes (§3.3). No behavior change to any shipping target — pure plumbing, verified by confirming every existing `targets/*.ini` environment still builds unchanged.
2. **Phase 2 — seam-free tests first:** write §4.2/§4.3/§4.8 (pure, no fakes needed) immediately against the native env as a smoke test that the harness itself works, before tackling the state-machine/loop() tests that need the fakes.
3. **Phase 3 — full coverage:** the rest of §4.1/§4.4–§4.14, against the fakes from Phase 1. Any case where the spec-derived assertion fails against current code is left failing and logged in §7 rather than "fixed" inline (per §1.5's ground rule) — surfaced for your review, since some may be stale spec, not code bugs.
4. **Phase 4 — cross-mirror + JS:** the fixture (§3.6), Python test file, and `follow.js` extraction + Node tests (§3.5/§4.15/§4.16).
5. **Phase 5 — CI wiring:** add the native test env and the Node test run as jobs in `.github/workflows/build.yml`, alongside (not replacing) the existing per-target firmware build matrix.
6. **Phase 6 — the actual cleanup refactor begins**, using this suite (green at that point) as the regression net it was built for.

---

## 7. Open findings for review (surfaced while drafting this spec, not resolved here)

### 7.1 `applyConfig()` doesn't enforce GVAR-index/RC-channel uniqueness; `follow.js` does

Detailed in §2.3. A direct `POST /followmanager/config` bypassing the web UI can currently set `statusGvarIndex == conditionFlagsGvarIndex`, or the same RC channel to two axes, and the firmware accepts it — the only guard is client-side. `mock_server.py`'s own comment suggests this was a deliberate choice ("UI-only... deliberately not duplicated"), but it's worth confirming that's still the intended posture before this spec's cross-mirror test (§4.15) locks in "JS is a strict superset, and that's fine" as permanent, tested behavior rather than an oversight.

### 7.2 §2.3's "Python mirrors only the C++ rules, JS is a strict superset" is incorrect — all three validators disagree in both directions

Discovered while building the §3.6 cross-mirror fixture (`docs/spec/fixtures/follow-config-cases.json`) and its three consumers (§4.15). Tracing all three `validate*`/`applyConfig()` implementations line-by-line against the same fixture found real divergence beyond §7.1's already-known gap:

- **`mock_server.py`'s `validate_config()` is missing two rules both C++ and JS have**: it never checks `targetPeer` against `NODES_MAX`, and — more significantly — it has **no `offsetGeometrySane()` equivalent at all**. A config with a geometrically-insane static offset (e.g. `ofsLongM=-2` against the default `minSepM=8`) passes Python validation outright. This isn't the documented "UI-only, deliberately not duplicated" case (§7.1) — geometry sanity is meant to be enforced everywhere; this looks like an oversight, not a choice.
- **`follow.js`'s `validateConfig()` is missing six rules C++ and Python both have**: no `targetPeer` range check, no GVAR-index range check (`-1` or `0-7`) for any of the four GVAR fields (it only checks *uniqueness* among them, not individual validity), no RC-channel range check (`-1` or `1-16`) for any of the four channel fields (same gap — uniqueness only), no `maxTargetSpeedMps > minTargetSpeedMps >= 0` check, and no `speedCorrectionAccelCmS2 >= 0` check. So JS is neither a strict superset nor subset of C++ — it adds four rules (§2.3's original finding) while lacking six others.

Net effect: of the ~17 distinct validation rules across the three implementations, only 5 (`emitHz`, `peerTimeoutMs`, the `minSepM`/`minVSepM`/`minAltM` non-negativity check, `maxTargetDistM`, `minCourseSpeed`) are actually enforced identically by all three. The fixture and its three test consumers (C++: `test/test_follow_native/test_cross_mirror_fixture.cpp`; Python: `.claude/skills/web-ui-preview/test_mock_server.py`; JS: `test/follow-logic.test.js`) encode each validator's *actual* current behavior per-case (`cppValid`/`pythonValid`/`jsValid`, independently) rather than a single shared `expectValid`, specifically so this divergence stays visible and tested instead of being silently assumed away. Worth a decision: is `mock_server.py`'s missing geometry check a bug to fix (mirror-of-a-mirror drift), or is loose client-tooling validation acceptable given it's dev-only?

### 7.3 Resolved (2026-09-03): all three validators now enforce the same rule set

The binary-size constraint that originally justified keeping §7.1/§7.2's gaps JS-only (or Python-loose) no longer applies now that the memory layout has grown — see the FollowManager EEPROM/flash budget discussion elsewhere in this repo's history. Given that, the open question in §7.2 ("bug to fix, or acceptable divergence?") is resolved in favor of fixing it: loose validation on any one surface was never a deliberate defense-in-depth choice, just a size trade-off that's no longer needed.

All ~17 rules are now implemented identically by `FollowManager::applyConfig()` (`FollowManager.cpp`), `mock_server.py`'s `validate_config()`, and `html/follow-logic.js`'s `validateConfig()`:

- §7.1/§7.2's four JS-only rules (GVAR-index uniqueness, RC-channel uniqueness, `autothrottleEnableRcChannel` distinct from the axis channels, `autothrottleEnableMaxThresholdUs > autothrottleEnableMinThresholdUs`) were added to `applyConfig()` and `validate_config()`.
- §7.2's `mock_server.py` gaps (`targetPeer` vs `NODES_MAX`, the missing `offsetGeometrySane()` equivalent) were added to `validate_config()`.
- §7.2's six `follow-logic.js` gaps (`targetPeer` range, GVAR-index range per field, RC-channel range per field, `maxTargetSpeedMps > minTargetSpeedMps >= 0`, `speedCorrectionAccelCmS2 >= 0`) were added to `validateConfig()`.

Since every case now produces the same verdict from all three validators, the fixture's per-validator `cppValid`/`pythonValid`/`jsValid` columns collapsed into a single `expectValid`, and the three test consumers (§4.15) were updated to assert against it. The per-case `note` fields were rewritten to record *what used to diverge and how it was fixed*, in place of the old "X_only" case-name suffixes that no longer apply.
