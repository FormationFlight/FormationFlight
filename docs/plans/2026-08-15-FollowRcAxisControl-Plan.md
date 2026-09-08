# FormationFlight — In-Flight RC Control of the Follow Slot — Implementation Plan

**Spec:** [`docs/spec/2026-08-15-FollowRcAxisControl.md`](../spec/2026-08-15-FollowRcAxisControl.md)
**Depends on:** [`2026-07-31-FollowMeOnInav-Plan.md`](2026-07-31-FollowMeOnInav-Plan.md) and [`2026-08-13-FollowStatusOsdGvar-Plan.md`](2026-08-13-FollowStatusOsdGvar-Plan.md) — both fully shipped. `FollowRuntimeConfig`, EEPROM persistence, `conditionFlagsGvarIndex`, and `updateStatusGvars()` all exist today and are extended, not built from scratch, by this plan.
**Status:** Draft for review

This is a bigger addendum than the OSD/GVAR one — it adds a new MSP read path, a new per-cycle validation layer with persistent state, and a signature change to an existing method (`updateStatusGvars(bool) → updateStatusGvars(int32_t)`). Sequenced into phases that each produce something bench-testable in isolation, since the geometry safety net (Phase 3) is the part most worth testing thoroughly before it's wired into the live control loop.

```
Phase 1 (MSP_RC polling)     Phase 2 (config model)
        \                         /
         \                       /
          Phase 3 (RC mapping + two-layer safety net, resolveOffset() rewrite)
           /                    \
          /                      \
 Phase 4 (altitude floor      Phase 5 (pre-arm advisory check)
  attribution, §5)                    |
          \                          /
           \                        /
            Phase 6 (status endpoint + WiFiManager POST + web UI)
                          |
            Phase 7 (bench-test pass)
```

---

## Corrections / notes against source (found during planning, not yet in the spec)

1. **Spec §2.1's "leaving the rest... untouched" claim is inverted, but its conclusion still holds.** Verified against `MSP::recv()` (`MSP.cpp:120-135`): on every successful, checksum-valid response, it explicitly zero-fills every byte of the caller's buffer from `recvSize` up to `maxSize` (`MSP.cpp:133-135`), it does not leave stale bytes in place. So a channel index the FC doesn't actually populate reads back as `0µs` on every fresh poll — not "whatever was there before." This still lands on the spec's own conclusion (§2.1/§3.2: an unpopulated channel reads `0`, which clamps to the `1000µs` endpoint and resolves to `-gap`), so no design changes — just worth fixing the reasoning if the spec text is revised later. The practically important part this confirms: reading a full 32-byte `msp_rc_t` (`MSP_MAX_SUPPORTED_CHANNELS=16 * sizeof(uint16_t)`) is safe regardless of how many channels the connected receiver protocol actually reports.
2. **A poll *failure* (timeout/bad checksum) is a different code path from a poll that succeeds with fewer channels than requested**, and only the former is where spec §2.1's "cache the last successfully parsed struct, don't zero it" behavior actually matters. `MSP::recv()` returns `false` before touching `payload` at all if the header/checksum loop times out (`MSP.cpp:104,123,139`) — so `getRcChannelUs()` (Phase 1) must skip the `memset`-on-failure pattern `local_altitude_cm()`/`getAnalogValues()` use today and instead just leave the cached `static msp_rc_t` as-is on a failed `msp->request()`. This is a deliberate deviation from those two existing accessors, not an oversight — call it out in the code comment so a future refactor doesn't "fix" it back to the zeroing pattern by analogy.
3. **No target `.ini` currently sets any `FOLLOW_*` build flag** (`grep -rl FOLLOW_STATUS_GVAR_INDEX targets/` and the equivalent for every other `FOLLOW_*` key returns nothing) — every target builds on `FollowConfig.h`'s compile-time defaults alone. Spec §9 item 7 ("add three new `build_flags` defaults... to config/target `.ini` files") is therefore a no-op for this codebase today, same conclusion the GVAR plan already reached for its two fields. Not listed as a work item below.
4. **`updateStatusGvars()`'s signature change is the one place this plan touches already-shipped, working code**, not just adds to it. Every existing call site in `loop()` (`FollowManager.cpp:318,325,361,371`) passes a `bool floorClamped` today; Phase 4 changes all four to pass a computed `int32_t conditionCode` instead. This is called out explicitly in Phase 4 below since it's the one part of this plan that isn't purely additive.

---

## Phase 1 — MSP_RC polling (`MSPManager`) [Completed]

No dependency on anything else in this plan; safe to build and bench-test standalone against a live FC's RC input before any `FollowManager` code changes.

**`src/lib/MSP/MSP.h`** — no changes. `msp_rc_t`/`MSP_RC`/`MSP_MAX_SUPPORTED_CHANNELS` already exist (`MSP.h:47,260,263-265`).

**`src/lib/MSP/MSPManager.h`** — add, near `local_altitude_cm()`:
```cpp
// Reads a single RC channel's value (µs) from the FC over a cached MSP_RC
// poll (spec docs/spec/2026-08-15-FollowRcAxisControl.md §2.1/§9).
// channel1Based is 1-16 (MSP_MAX_SUPPORTED_CHANNELS). Returns false (and
// leaves *outUs untouched) only if not connected to a flight controller or
// channel1Based is out of range. A transient poll miss on an otherwise-
// connected FC is NOT one of those failure cases — it returns the last
// successfully parsed value from the cache instead, same reasoning
// local_altitude_cm()'s cache already rides out a single dropped frame.
bool getRcChannelUs(uint8_t channel1Based, uint16_t *outUs);
```

**`src/lib/MSP/MSPManager.cpp`** — add, next to `local_altitude_cm()`:
```cpp
// Cached MSP_RC poll (~100ms, matching local_altitude_cm()'s cadence) so
// FollowManager::loop() at FOLLOW_EMIT_HZ always sees a fresh-enough read
// (spec §3.3). Deliberately does NOT memset-on-failure the way
// local_altitude_cm()/getAnalogValues() do (see this plan's corrections
// note #2) — a dropped MSP_RC frame must not read as "channel near zero,"
// so a failed request just leaves the last successfully parsed struct in
// place. Polling only ever happens because a caller asked for a specific
// channel, so a pilot with no axis RC-assigned costs zero extra MSP
// traffic (spec §3.3) without this function needing its own enable flag.
bool MSPManager::getRcChannelUs(uint8_t channel1Based, uint16_t *outUs)
{
    static msp_rc_t rc = {};
    static unsigned long cached = 0;

    if (channel1Based < 1 || channel1Based > MSP_MAX_SUPPORTED_CHANNELS)
    {
        return false;
    }
    if (!hostIsFlightController(this->getFCVariant()))
    {
        return false;
    }

    if (millis() - cached >= 100)
    {
        if (msp->request(MSP_RC, &rc, sizeof(rc)))
        {
            cached = millis();
        }
        // Poll miss: `rc` is left exactly as it was (MSP::recv() only
        // touches the buffer on a checksum-valid response, MSP.cpp:104-144).
    }

    *outUs = rc.channelValue[channel1Based - 1];
    return true;
}
```

*Test (bench):* with a bench FC connected and a transmitter bound, call `getRcChannelUs(N, &us)` for a channel the receiver actually populates and confirm `us` tracks stick movement 1000-2000. Call it for a channel index beyond what the receiver reports and confirm it reads `0` (not garbage). Briefly disconnect/reconnect the MSP link mid-poll and confirm the last good value is retained rather than snapping to `0` on a single dropped frame.

---

## Phase 2 — Configuration model [Completed]

Depends on nothing but existing infrastructure; can be built in parallel with Phase 1.

**`src/lib/Follow/FollowConfig.h`** — add, next to the GVAR index defaults:
```cpp
// RC axis control (spec docs/spec/2026-08-15-FollowRcAxisControl.md §6),
// 1-based MSP_RC channel number per axis, or -1 to disable (default — zero
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
```

**`src/lib/Follow/FollowManager.h`**
- `FollowRuntimeConfig`, next to the GVAR fields:
  ```cpp
  // RC axis control (spec §6): 1-based MSP_RC channel per axis, or -1 =
  // disabled. Range enforced in applyConfig() (-1 or 1-16). The configured
  // ofs{Long,Lat,Vert}M value becomes that axis's live-scaled bound once a
  // channel is assigned (spec §2.2), not a fixed point.
  int16_t rcLongChannel = FOLLOW_RC_LONG_CHANNEL;
  int16_t rcLatChannel = FOLLOW_RC_LAT_CHANNEL;
  int16_t rcVertChannel = FOLLOW_RC_VERT_CHANNEL;
  ```
- `FollowEepromRecord`: add the same three `int16_t` fields (straight channel numbers, no narrowing conversion needed — see spec §6). Bump `#define FOLLOW_EEPROM_VERSION 3` to `4`.
- Private state, next to `lastSentConditionFlagsGvarValue` etc.:
  ```cpp
  // "Last known good" RC-scaled offset triple (spec §4.4) — the freeze
  // target when a candidate fails either safety layer. Bootstrapped to the
  // compile-time static offset, matching FollowRuntimeConfig's own member
  // initializers; applyConfig() resets this to the new config's static
  // offset on every successful apply (spec §4.4's reset rule).
  FollowOffset lastKnownGood{FOLLOW_OFS_LONG_M, FOLLOW_OFS_LAT_M, FOLLOW_OFS_VERT_M};
  // Whether resolveOffset()'s last cycle held lastKnownGood frozen rather
  // than adopting a fresh candidate (spec §4.4/§4.5), for statusJson()/
  // updateStatusGvars().
  bool rcSlotFrozen = false;
  // §4.6's ground-only advisory result, recomputed every loop() cycle while
  // disarmed and reset to false every cycle otherwise (spec §7's gating).
  bool rcPreArmCheckFailed = false;
  // Offset triple actually used for the last emitted waypoint (spec §7
  // liveOffset), distinct from lastKnownGood which persists even on a
  // targetSane() rejection where no waypoint went out.
  FollowOffset lastLiveOffset{};
  ```
- Private methods, next to `resolveOffset()`:
  ```cpp
  // Per-axis RC-to-offset mapping (spec §3.1/§3.2). channel1Based < 1 means
  // "no channel assigned" -> returns configuredM unchanged (the only
  // fallback case). Once a channel is assigned, whatever value comes back
  // is mapped unconditionally, including a raw reading below 1000us -- it
  // just clamps to the 1000 endpoint like any other out-of-range value
  // (spec §3.1/§3.2). getRcChannelUs() returning false (no FC connected, or
  // an out-of-MSP_RC-range channel number) is the one case where there's
  // genuinely no value to map, so that also falls back to configuredM.
  double resolveAxisOffset(double configuredM, int16_t channel1Based) const;
  bool anyRcChannelAssigned() const;
  ```
- Change `updateStatusGvars(bool floorClamped)`'s declaration to `updateStatusGvars(int32_t conditionCode)` (spec §9 item 4 — see Phase 4's corrections note).

**`src/lib/Follow/FollowManager.cpp`** — `applyConfig()` (`FollowManager.cpp:523-586`):
- Range validation, alongside the existing GVAR index checks:
  ```cpp
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
  ```
  (No duplicate-channel check — spec §6 explicitly keeps that front-end-only, same precedent as the GVAR index collision guard.) Requires `#include "../MSP/MSP.h"` in `FollowManager.cpp` for `MSP_MAX_SUPPORTED_CHANNELS`, or just hardcode `16` with a comment — implementer's call, the GVAR plan's precedent (`FollowManager.cpp:558-567`) uses a literal `7` rather than pulling in a GVAR-count constant, so a literal `16` with a one-line comment matches house style better than a new include.
- After `config = newConfig;`, add the lastKnownGood reset (spec §4.4):
  ```cpp
  config = newConfig;
  // §4.4: a config change (new gaps, a reassigned channel) can make the
  // previously-frozen triple meaningless, so re-anchor it to the new
  // static offset — the one point applyConfig() already guarantees is
  // geometry-sane via the offsetGeometrySane() check above.
  lastKnownGood = { config.ofsLongM, config.ofsLatM, config.ofsVertM };
  if (targetPeerChanged)
  {
      forceReacquire();
  }
  ```
- `toEepromRecord()`/`fromEepromRecord()`: add straight-through `int16_t` copies for `rcLongChannel`/`rcLatChannel`/`rcVertChannel`, same pattern as `statusGvarIndex`.
- `configJson()`: add
  ```cpp
  (*doc)["rcLongChannel"] = config.rcLongChannel;
  (*doc)["rcLatChannel"] = config.rcLatChannel;
  (*doc)["rcVertChannel"] = config.rcVertChannel;
  ```

*Test (bench):* POST a config with `rcVertChannel=99` via curl/the eventual UI, confirm 400 with the range error. POST `rcVertChannel=5`, confirm it round-trips through `GET /followmanager/config`, survives "Save to EEPROM" + reboot. Confirm a fresh/never-saved unit still boots on compile-time `-1` defaults (EEPROM version mismatch path, unchanged from today).

---

## Phase 3 — RC-to-offset mapping + two-layer geometry safety net [Completed]

Depends on Phase 1 (`getRcChannelUs`) and Phase 2 (the three config fields + `lastKnownGood`/`rcSlotFrozen` state). This is the phase worth the most bench-test attention before Phase 6 wires it into a UI a pilot can actually fly with.

**`src/lib/Follow/FollowManager.cpp`** — new free functions, next to `offsetGeometrySane()` (`FollowManager.cpp:257-277`), which Layer 1 reuses unchanged:

```cpp
// Layer 2 of spec §4: true if `axis` crossing from `referenceAxis`'s sign
// to `candidateAxis`'s sign is unsafe right now. Only a genuine sign flip
// counts as "crossing" (spec §4.3 condition 1) — 0 on either side is the
// boundary itself, not a side, so it never counts as a flip. `coMag` is
// the *smaller* of the other two axes' combined magnitude at the
// reference point and at the candidate point (spec §4.3's conservative
// choice, covering two RC-assigned axes swinging in the same cycle).
static bool axisSignLocked(double candidateAxis, double referenceAxis,
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

// Both safety layers (spec §4.2 Layer 1, §4.3 Layer 2), evaluated
// together so there's exactly one pass/fail test. Shared, read-only,
// between resolveOffset() (which adopts `candidate` as the new
// lastKnownGood on a pass) and the §4.6 pre-arm advisory check (which
// never mutates state) — both callers must always agree on the same
// answer for the same inputs.
static bool candidateOffsetOk(const FollowOffset &candidate, const FollowOffset &reference,
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
```

`FollowManager::resolveAxisOffset()` (spec §3.1/§3.2):
```cpp
double FollowManager::resolveAxisOffset(double configuredM, int16_t channel1Based) const
{
    if (channel1Based < 1)
    {
        return configuredM; // no channel assigned (spec §3.2)
    }

    uint16_t us;
    if (!MSPManager::getSingleton()->getRcChannelUs((uint8_t)channel1Based, &us))
    {
        return configuredM; // no FC connected, or channel1Based out of MSP_RC's range (spec §3.2)
    }
    // Whatever value comes back is mapped as-is, including below 1000us --
    // there is no separate "invalid reading" case once a channel is
    // assigned (spec §3.2). The clamp below already guards over/under-
    // travel, so this also covers an unpopulated channel index (reads 0us
    // from the cached msp_rc_t, spec §2.1) the same way: it resolves to
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
```

`FollowManager::resolveOffset()` replaces its current one-line body:
```cpp
FollowOffset FollowManager::resolveOffset()
{
    FollowOffset candidate = {
        resolveAxisOffset(config.ofsLongM, config.rcLongChannel),
        resolveAxisOffset(config.ofsLatM, config.rcLatChannel),
        resolveAxisOffset(config.ofsVertM, config.rcVertChannel),
    };

    bool ok = candidateOffsetOk(candidate, lastKnownGood, config.minSepM, config.minVSepM);
    rcSlotFrozen = !ok;
    if (ok)
    {
        lastKnownGood = candidate;
    }
    // On failure, lastKnownGood is left exactly as it was — the freeze
    // (spec §4.4). When no axis has an RC channel assigned, `candidate`
    // always equals lastKnownGood already (both are the static config,
    // which applyConfig() guarantees is geometry-sane), so ok is always
    // true and this is a no-op — today's plain static-offset behavior,
    // unchanged.
    return lastKnownGood;
}
```

*Test (bench, no aircraft needed — the parent plan's `/peermanager/spoof` tooling drives a peer):*
1. No RC channels assigned: confirm `resolveOffset()` behaves exactly as before this phase (regression check against the parent spec's existing behavior).
2. One axis assigned (e.g. vertical), gap large enough that the whole range is valid on both sides (e.g. 20m gap, minVSepM 13m only matters when horizontal ~0 — so pick a slot with nonzero lateral to sidestep the stacked rule for this first test): sweep the channel 1000→2000, confirm `liveOffset`/the emitted altitude tracks linearly, `-gap` at 1000, `0` at 1500, `+gap` at 2000.
3. Fully stacked slot (long=lat=0, vert assigned, gap ≥ minVSepM): sweep vertical channel slowly from full-below to full-above, confirm the offset freezes somewhere in the middle (the invalid band) and does not jump — `rcSlotFrozen` should read true throughout the invalid band.
4. Same stacked setup, but with a second axis (e.g. lateral) also RC-assigned: widen lateral past `minSepM` first, then cross vertical through center, then bring lateral back — confirm the crossing succeeds this time and `rcSlotFrozen` clears.
5. Channel reporting `<1000µs` (transmitter failsafe or a genuine stick position below the nominal range): confirm that axis clamps to the `1000` endpoint and resolves to `-gap`, exactly like a stick held fully at that extreme — it must *not* fall back to the plain configured value (spec §3.2's removed fallback). Separately, assign an axis to a channel index the receiver never actually populates and confirm it likewise pins at `-gap` (reads `0µs` from the cache, clamps the same way) rather than reading as the static default.

---

## Phase 4 — Altitude floor attribution (spec §5) + `updateStatusGvars()` signature change [Completed]

Depends on Phase 3 (`rcSlotFrozen`). Touches `loop()`, the one place in this plan that modifies already-shipped, working control-loop code rather than purely adding to it.

**`src/lib/Follow/FollowManager.cpp`** — `updateStatusGvars()` (`FollowManager.cpp:434-469`): change the parameter and drop the old floor-only mapping:
```cpp
void FollowManager::updateStatusGvars(int32_t conditionCode)
{
    ...
    if (config.conditionFlagsGvarIndex >= 0)
    {
        int32_t value = conditionCode; // spec §5.3's 0/1/2 table, computed by callers now
        bool due = lastSentConditionFlagsGvarValue == INT32_MIN
                 || value != lastSentConditionFlagsGvarValue
                 || (now - lastConditionFlagsGvarSendMs) >= FOLLOW_GVAR_HEARTBEAT_MS;
        ...
    }
}
```

`loop()` (`FollowManager.cpp:300-377`) — the floor-clamp block (`FollowManager.cpp:346-357`) gains the static-offset comparison (spec §5.1), and a new conditionCode combines it with `rcSlotFrozen` (spec §5.2/§5.3) before both remaining call sites:
```cpp
int32_t floorCm = (int32_t)lround(config.minAltM * 100.0);
bool floorClamped = altCm < floorCm;
if (floorClamped)
{
    altCm = floorCm;
}

// spec §5.1: attribute the clamp to RC only if the plain configured
// (non-RC-scaled) vertical offset would NOT also have clamped. When no
// channel is assigned to vertical, offset.vertical_m == config.ofsVertM
// by construction (§3.2), so this always agrees with the "actual" check
// and degrades to today's plain floor-clamp behavior with no special-casing.
bool floorAttributableToRc = false;
if (floorClamped)
{
    double altCmStaticD = (double)MSPManager::getSingleton()->local_altitude_cm()
                         + (double)peer->relalt * 100.0
                         + config.ofsVertM * 100.0;
    int32_t altCmStatic = (int32_t)lround(altCmStaticD);
    floorAttributableToRc = altCmStatic >= floorCm;
}

// spec §5.3's condition-code table: code 2 whenever either mechanism is
// active (they never disagree — see spec §5.2), otherwise code 1 if just
// the floor clamped for a reason unrelated to RC, otherwise 0.
int32_t conditionCode = 0;
if (floorClamped)
{
    conditionCode = floorAttributableToRc ? 2 : 1;
}
if (rcSlotFrozen)
{
    conditionCode = 2;
}

if (!targetSane(offset, target))
{
    updateStatusGvars(conditionCode);
    return;
}

int16_t headingDeg = resolveHeadingDeg(peer, courseDeg);
MSPManager::getSingleton()->sendFollowWaypoint(target.lat_1e7, target.lon_1e7, altCm, headingDeg);
updateStatusGvars(conditionCode);

haveLastTarget = true;
lastTarget = target;
lastTargetAltCm = altCm;
lastTargetTime = millis();
lastLiveOffset = offset; // spec §7 liveOffset
```
The other two existing call sites (`FollowManager.cpp:318`, gate-inactive; `FollowManager.cpp:325`, ACQUIRING/LOCKED_HOLDING) change from `updateStatusGvars(false)` to `updateStatusGvars(0)` for the ACQUIRING/LOCKED_HOLDING one (no waypoint computed this cycle, nothing RC or floor-related to report — unchanged behavior, just the new call shape). The gate-inactive call site is replaced entirely in Phase 5 below.

*Test (bench):* reuse the parent plan's still-outstanding altitude-floor test setup (spoof a leader descending toward the follower's floor). With `rcVertChannel` unassigned, confirm the condition GVAR reports `1` exactly as it does today (regression check). Assign `rcVertChannel` and drive it toward its negative extreme while the leader is at a normal altitude (floor not otherwise at risk), confirm the GVAR reports `2` once the RC-driven vertical push alone clamps the altitude, and `1` if you then also make the leader itself descend low enough that the static offset would have clamped anyway.

---

## Phase 5 — Pre-arm advisory check (spec §4.6) [Completed]

Depends on Phase 3 (`candidateOffsetOk`/`resolveAxisOffset` shared helpers). Replaces the gate-inactive call site's hardcoded `updateStatusGvars(false)`.

**`src/lib/Follow/FollowManager.cpp`** — `loop()`'s gate-inactive branch (`FollowManager.cpp:312-320`):
```cpp
if (!followSwitchActive())
{
    state = FOLLOW_LOCK_IDLE;
    lockedId = 0;
    lockedName[0] = '\0';
    haveValidCourse = false;

    // spec §4.6: catch the common "RC disagrees with the static default's
    // sign" bootstrap trap (spec §10) while still on the ground, where the
    // pilot can simply move the stick before it matters mid-flight. Runs
    // only while disarmed; reset to false every other cycle so it never
    // reports stale while armed (spec §7's gating for rcPreArmCheckFailed).
    int32_t conditionCode = 0;
    rcPreArmCheckFailed = false;
    if (MSPManager::getSingleton()->getState() == 0 && anyRcChannelAssigned())
    {
        FollowOffset candidate = {
            resolveAxisOffset(config.ofsLongM, config.rcLongChannel),
            resolveAxisOffset(config.ofsLatM, config.rcLatChannel),
            resolveAxisOffset(config.ofsVertM, config.rcVertChannel),
        };
        // Read-only: deliberately does not touch lastKnownGood (spec
        // §4.6's "never write to lastKnownGood" requirement) — this is a
        // simulation of "what would happen if follow engaged right now,"
        // not a real state transition.
        rcPreArmCheckFailed = !candidateOffsetOk(candidate, lastKnownGood, config.minSepM, config.minVSepM);
        conditionCode = rcPreArmCheckFailed ? 2 : 0;
    }

    updateStatusGvars(conditionCode);
    return;
}
```

*Test (bench):* with the craft disarmed, set a stacked-ish slot and assign a channel to the vertical axis; move the transmitter stick to the side that disagrees with the configured static default's sign. Confirm `rcPreArmCheckFailed`/the condition GVAR flip to the warning state while disarmed. Arm (or simulate `getState()` returning armed) and confirm the field/GVAR stops updating (freezes at its last disarmed value per spec, or drop to `false` per the reset-every-cycle logic above — verify against the actual armed-and-gate-inactive case, e.g. armed but GCS NAV not yet engaged, which per spec should keep reporting `0`/`false`, matching "as today"). Confirm centering the stick (or widening the other axis) before arming clears the warning.

**Post-implementation correction (found in an 8-angle code review after this phase shipped):** the code block above — copied faithfully from this plan — nests the pre-arm check inside `loop()`'s `!followSwitchActive()` branch, gating it on "gate inactive" rather than on arm state alone. That contradicts spec §4.6's explicit "Runs **independent of the follow gate/`GCS NAV`**" requirement: a pilot who flips the GCS NAV switch on before arming (a normal pre-flight order) would make `followSwitchActive()` true while still disarmed, so this block would stop executing and `rcPreArmCheckFailed` would freeze at its last value for the rest of the session instead of continuing to track arm state. Fixed by hoisting the `getState() == 0 && anyRcChannelAssigned()` check to the top of `loop()`, before the gate check, gated purely on arm state — see `FollowManager.cpp`'s `loop()` for the corrected version. The gate-inactive branch now just reads `rcPreArmCheckFailed` (already computed above it) into `updateStatusGvars(rcPreArmCheckFailed ? 2 : 0)` rather than recomputing it. The candidate-construction duplication between this block and `resolveOffset()` was also factored into a shared `resolveCandidateOffset()` helper.

---

## Phase 6 — Status endpoint, config POST, and web UI [Completed]

Depends on Phases 2-5 (all the fields/state this phase surfaces). The three sub-parts (statusJson/configJson, WiFiManager POST params, follow.js panel) have no dependency on each other and can be built in parallel.

### 6a. `src/lib/Follow/FollowManager.cpp` — `statusJson()` (`FollowManager.cpp:391-413`)

```cpp
if (haveLastTarget)
{
    JsonObject live = doc->createNestedObject("liveOffset");
    live["longM"] = lastLiveOffset.longitudinal_m;
    live["latM"] = lastLiveOffset.lateral_m;
    live["vertM"] = lastLiveOffset.vertical_m;
    (*doc)["rcSlotFrozen"] = rcSlotFrozen;
}
(*doc)["rcPreArmCheckFailed"] = rcPreArmCheckFailed;
```
(`rcPreArmCheckFailed` is unconditional — it's already `false` by construction whenever armed or no axis assigned, per Phase 5's every-cycle reset, so no extra gating needed here.)

### 6b. `src/lib/WiFi/WiFiManager.cpp` — `handleFollowManagerConfigPost()` (`WiFiManager.cpp:301-354`)

Add, alongside the GVAR index params:
```cpp
if (request->hasParam("rcLongChannel", true)) cfg.rcLongChannel = (int16_t)strParam("rcLongChannel").toInt();
if (request->hasParam("rcLatChannel", true)) cfg.rcLatChannel = (int16_t)strParam("rcLatChannel").toInt();
if (request->hasParam("rcVertChannel", true)) cfg.rcVertChannel = (int16_t)strParam("rcVertChannel").toInt();
```

### 6c. `AsyncJsonDocument` capacity bumps (`src/lib/WiFi/WiFiManager.cpp:166,173`)

`/followmanager/status`'s `StaticJsonDocument<512>` gains a nested 3-double object plus two bools; `/followmanager/config`'s `StaticJsonDocument<768>` gains three more `int16_t` fields. Neither addition is large, but bump both (e.g. `640`/`896`) and let `ArduinoJson`'s own overflow assertion (or a quick bench check of the actual serialized response) confirm the new sizes are sufficient rather than guessing exactly — same "verify, don't guess" approach the GVAR plan's own additions used.

### 6d. `html/follow.js`

- New options array, next to `gvarIndexOptions`:
  ```js
  const rcChannelOptions = [[-1, 'Disabled']].concat(Array.from({length: 16}, (_, i) => [i + 1, String(i + 1)]));
  ```
- `validateConfig()` — duplicate-channel guard, mirroring the GVAR collision check:
  ```js
  const rcChannels = [cfg.rcLongChannel, cfg.rcLatChannel, cfg.rcVertChannel].filter(c => c !== -1);
  if (new Set(rcChannels).size !== rcChannels.length) {
    return { section: 'rc', message: 'Each RC axis must use a different channel (or Disabled)' };
  }
  ```
- New panel, modeled on "OSD Status (GVAR)" (`follow.js:284-294`):
  ```js
  <div class="py-1 divide-y border rounded bg-white flex flex-col">
    <div class="font-light uppercase flex items-center text-gray-600 px-4 py-2">
      RC Axis Control
    <//>
    <div class="py-2 px-5 flex-1 flex flex-col relative">
      ${sectionError('rc')}
      <div class="text-xs text-gray-500 mb-2">Once an axis has a channel assigned, its configured gap becomes a live-adjustable range (stick centered = centered slot, full deflection = the configured gap in that direction) rather than a fixed point.</div>
      <${Setting} title="Longitudinal Channel" tip="RC channel that live-adjusts the longitudinal (ahead/behind) slot between -Gap and +Gap. Disabled uses the fixed configured value." value=${config.rcLongChannel} setfn=${mksetfn('rcLongChannel')} type="select" options=${rcChannelOptions} />
      ${config.rcLongChannel !== -1 && config.ofsLongM === 0 && html`<div class="text-xs text-yellow-700 mb-2">Longitudinal Gap is 0 — this channel currently has no effect.<//>`}
      <${Setting} title="Lateral Channel" tip="RC channel that live-adjusts the lateral (left/right) slot between -Gap and +Gap." value=${config.rcLatChannel} setfn=${mksetfn('rcLatChannel')} type="select" options=${rcChannelOptions} />
      ${config.rcLatChannel !== -1 && config.ofsLatM === 0 && html`<div class="text-xs text-yellow-700 mb-2">Lateral Gap is 0 — this channel currently has no effect.<//>`}
      <${Setting} title="Vertical Channel" tip="RC channel that live-adjusts the vertical (above/below) slot between -Gap and +Gap." value=${config.rcVertChannel} setfn=${mksetfn('rcVertChannel')} type="select" options=${rcChannelOptions} />
      ${config.rcVertChannel !== -1 && config.ofsVertM === 0 && html`<div class="text-xs text-yellow-700 mb-2">Vertical Gap is 0 — this channel currently has no effect.<//>`}
      <div class="text-xs text-gray-500 mt-2">Crossing a stacked or in-line axis from one side of the leader to the other requires first widening one of the other two RC-assigned axes past Min Separation — the slot won't fly through the leader to get there. This is expected behavior, not a bug.</div>
      ${status.rcSlotFrozen && html`<div class="bg-yellow-50 border border-yellow-200 text-yellow-800 rounded-md px-3 py-2 text-sm mt-2">Slot is currently frozen at its last safe position — current RC input would produce an unsafe slot.<//>`}
      ${status.liveOffset && html`<div class="text-xs text-gray-500 mt-2">Live offset: ${status.liveOffset.longM.toFixed(1)}m long, ${status.liveOffset.latM.toFixed(1)}m lat, ${status.liveOffset.vertM.toFixed(1)}m vert<//>`}
    <//>
  <//>

  ${status.rcPreArmCheckFailed && html`
  <div class="lg:col-span-2 py-1 border rounded bg-white flex flex-col">
    <div class="py-2 px-5 flex-1 flex flex-col relative">
      <div class="bg-red-50 border border-red-200 text-red-800 rounded-md px-4 py-2 text-sm flex items-center gap-2">
        <${Icons.info} class="w-5 h-5 shrink-0" />
        Current RC stick/channel positions would produce an unsafe slot the instant follow engages. Center your sticks (or widen another RC-assigned axis) before arming.
      <//>
    <//>
  <//>
  `}
  ```
- `applyLive()`'s POST body: add
  ```js
  body.append('rcLongChannel', config.rcLongChannel);
  body.append('rcLatChannel', config.rcLatChannel);
  body.append('rcVertChannel', config.rcVertChannel);
  ```

*Test (UI, use the `web-ui-preview` skill to bench this without hardware):* assign the same channel to two axes, confirm Save is blocked with the inline error; assign distinct channels, confirm it saves. Confirm the zero-gap hint appears/disappears correctly as gap values change. Manually POST a status payload (or drive it via `/peermanager/spoof` + a real bench FC) with `rcSlotFrozen: true` and confirm the inline warning renders; same for `rcPreArmCheckFailed` and the banner.

---

## Phase 7 — Bench-test pass (full-stack, spec §12-style acceptance) [Build-verified; hardware bench pass outstanding]

Implementation for Phases 1-6 compiles cleanly (`pio run` against an ESP32 target with WiFi and an ESP8266 target) and the config/status JSON round-trips correctly (including validation) through the `web-ui-preview` mock backend. The actual bench-test items below require a real bench INAV FC with a bound transmitter/receiver and are not yet run — still outstanding.

Run after Phases 1-6 are all merged. Requires a bench INAV FC with a bound transmitter/receiver (for real RC values) and the parent plan's `/peermanager/spoof` tooling (for driving the peer-lock state machine without a second aircraft).

1. **End-to-end mapping:** assign all three axes to distinct channels, confirm each axis's live offset tracks its channel 1000→2000 → `-gap..+gap` independently, with the other two axes unaffected.
2. **Geometry freeze, single-axis stacked crossing:** as in Phase 3's test 3/4, now observed through the web UI's `liveOffset`/`rcSlotFrozen` readout instead of raw internal state.
3. **Altitude floor attribution:** Phase 4's test, confirmed end-to-end through the OSD GVAR (requires an INAV 9.0+ bench FC per the parent GVAR plan's version gating) as well as the web UI.
4. **Pre-arm advisory:** Phase 5's test, confirmed through both `/followmanager/status`'s `rcPreArmCheckFailed` and the web UI banner.
5. **EEPROM persistence + version bump:** set all three channels and confirm they survive Save-to-EEPROM + reboot. Confirm a unit with Follow config saved under the old `FOLLOW_EEPROM_VERSION == 3` layout resets to compile-time defaults on first boot post-upgrade (documented, expected consequence — same as the 2→3 bump before it).
6. **Zero added traffic when unused:** with all three channels `-1` (default), confirm via a bus tap/log that no `MSP_RC` requests are ever sent — the pilot who doesn't use this feature sees no behavior change and no extra MSP load.
7. **RX failsafe behavior (spec §11):** with a channel assigned, trigger the transmitter/receiver's RX failsafe deliberately (bench-safe way — e.g. power off the transmitter with the receiver bound) and confirm FF just keeps responding to whatever the FC reports (per spec, this is expected/undefined-by-design, not a regression to chase) — the real backstop is the follow-mode gate dropping out on the underlying GCS NAV loss, not this feature.

---

## Files touched

| File | Purpose |
|---|---|
| `src/lib/MSP/MSPManager.{h,cpp}` | `getRcChannelUs()`, cached `MSP_RC` poll |
| `src/lib/Follow/FollowConfig.h` | `FOLLOW_RC_{LONG,LAT,VERT}_CHANNEL` defaults |
| `src/lib/Follow/FollowManager.{h,cpp}` | RC mapping, two-layer safety net, `lastKnownGood`/`rcSlotFrozen` state, altitude-floor attribution, pre-arm check, `updateStatusGvars()` signature change, config/EEPROM fields, `configJson()`/`statusJson()` additions |
| `src/lib/WiFi/WiFiManager.cpp` | three new POST params, two `StaticJsonDocument` capacity bumps |
| `html/follow.js` | RC Axis Control panel, collision validation, freeze/pre-arm UI |

No changes to `src/main.cpp`, `src/lib/ConfigHandler.cpp`, `MSP.h`, or any `targets/*.ini` (see corrections note #3).

---

## Open items carried over from the spec, not resolved by this plan

Per spec §10, these are explicitly deferred design questions, not implementation gaps in the phases above — flagging here so they aren't mistaken for missed work:
- No deadband/hysteresis on the freeze — it can chatter at a boundary. Revisit only if bench/flight testing shows it's disruptive.
- Layer 2's conservative co-magnitude test is not full three-axis path/segment collision math — a pathological simultaneous three-channel move could in principle still thread a jump it doesn't catch.
- The "bootstrap trap" (§10's last item) is mitigated by Phase 5's pre-arm check but not fixed at the mechanism level — a pilot who arms anyway with the warning showing still experiences a possible first-cycle sign-lock away from their actual stick position.
