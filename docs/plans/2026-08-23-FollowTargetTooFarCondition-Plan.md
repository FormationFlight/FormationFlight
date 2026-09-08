# FormationFlight — `TARGET_TOO_FAR` Condition Code — Implementation Plan

**Extends:** [`docs/spec/2026-08-13-FollowStatusOsdGvar.md`](../spec/2026-08-13-FollowStatusOsdGvar.md) §3.2's
`conditionFlagsGvarIndex` extension point, and the condition-code table
[`docs/spec/2026-08-15-FollowRcAxisControl.md`](../spec/2026-08-15-FollowRcAxisControl.md) §5.3 built on top of it.
No new spec doc — this is a small, self-contained addendum to an extension point both specs already anticipated
("future non-exclusive conditions... code `2`, `3`, etc."), so the design decisions live in this plan directly.
**Status:** Draft for review

## What's changing

Today, `FollowManager::loop()` silently drops the commanded waypoint whenever the solved target is farther than
`config.maxTargetDistM` from the follower (`FollowManager.cpp:432-451`, inside `targetSane()`) — the pilot gets no
OSD indication of *why* follow went quiet, only whatever floor/RC condition code happened to already be set (or
`NONE`). This adds a dedicated `FOLLOW_CONDITION_TARGET_TOO_FAR` code so that case is distinguishable on the OSD,
and cleans up how `conditionCode` is assembled in `loop()` so a fourth condition is a one-line addition later
instead of another bespoke `if`/`else` to thread through by hand.

## Decisions (confirmed with the user before writing this plan)

1. **Wire values are renumbered, not appended.** `FOLLOW_CONDITION_RC_INVALID_GAP_SETTINGS` moves from `2` to `3`;
   `FOLLOW_CONDITION_TARGET_TOO_FAR` takes the now-open `2`, sitting between `FLOOR_CLAMPED` (`1`) and
   `RC_INVALID_GAP_SETTINGS` (`3`) so the enum's numeric order tracks pilot-relevant priority monotonically going
   forward. **This is a breaking change** for anyone who already wired an INAV Logic Condition against the old
   `conditionFlagsGvarIndex == 2` meaning ("RC-driven slot frozen") — call it out prominently in
   `docs/user-guide-follow-mode.md` (work item 3) and in the PR/release notes.
2. **`offsetGeometrySane()` failures are untouched.** `targetSane()` today conflates two unrelated rejection
   reasons — bad offset geometry vs. `minSepM`/`minVSepM`, and the solved target being too far — into one bool.
   Only the distance check is "target too far"; the geometry-sane branch keeps today's behavior (report whatever
   floor/RC code was already computed, or `NONE`) and does not get its own condition code in this pass.

## Priority model: `raiseCondition()` instead of sequential overwrites

`loop()` currently builds `conditionCode` by assigning it multiple times in sequence — `floorClamped` sets it,
then `rcSlotFrozen` unconditionally overwrites it (`FollowManager.cpp:559-567`). That "last write wins" pattern
only happens to be correct today because the two checks are written in priority order by coincidence; it doesn't
scale to a third condition whose priority sits *between* the other two, and every future addition would need
readers to re-verify the whole `if` chain's ordering by hand.

Replacing it: since the enum's numeric values are now priority-ordered (decision 1 above), precedence reduces to
"keep the highest value seen this cycle." A tiny local helper captures that:

```cpp
FollowConditionCode conditionCode = FOLLOW_CONDITION_NONE;
auto raiseCondition = [&conditionCode](FollowConditionCode candidate) {
    if (candidate > conditionCode) conditionCode = candidate;
};
```

Each detection site calls `raiseCondition(X)` instead of assigning `conditionCode` directly. Order of the calls in
source no longer matters, and a future condition is just another `raiseCondition(FOLLOW_CONDITION_WHATEVER)` call
wherever it's detected — no re-reading of surrounding `if`/`else` branches required to confirm it won't
accidentally get clobbered by something below it.

## Work items

### 1. `src/lib/Follow/FollowManager.h`

Reorder/extend the enum and update its comment:

```cpp
// spec docs/spec/2026-08-13-FollowStatusOsdGvar.md §3.2's condition-code
// table, sent via conditionFlagsGvarIndex. Sequential, not a bitmask — only
// one code is ever reported at a time. Values are ordered low-to-high by
// pilot-relevant priority (see raiseCondition() in FollowManager.cpp's
// loop()): when multiple conditions are true in the same cycle, the
// highest-valued one wins. Append future conditions above
// FOLLOW_CONDITION_RC_INVALID_GAP_SETTINGS in priority order — renumbering
// an existing value (as this file's 2026-08-23 change did to make room for
// TARGET_TOO_FAR) breaks any pilot's INAV Logic Condition already wired
// against the old number, so prefer appending unless priority genuinely
// demands an insert.
enum FollowConditionCode {
    FOLLOW_CONDITION_NONE = 0,                    // no condition active
    FOLLOW_CONDITION_FLOOR_CLAMPED = 1,            // altitude floor clamped, unrelated to RC
    FOLLOW_CONDITION_TARGET_TOO_FAR = 2,           // solved target beyond maxTargetDistM; waypoint suppressed
    FOLLOW_CONDITION_RC_INVALID_GAP_SETTINGS = 3,  // RC-attributable invalid gap settings, and/or rcSlotFrozen
};
```

Replace the `targetSane()` declaration with a narrower `targetTooFar()` (geometry-sane stays a direct call to the
existing free function `offsetGeometrySane()`, already used elsewhere in the file):

```cpp
// Runtime sanity: is the solved target farther than config.maxTargetDistM
// from the follower's own position (spec §7.4)? Geometry sanity
// (offsetGeometrySane()) is checked separately by the caller so it can
// attribute a failure here specifically to FOLLOW_CONDITION_TARGET_TOO_FAR.
bool targetTooFar(const FollowTarget &target) const;
```

Update `updateStatusGvars()`'s doc comment (currently says "spec §5.3 0/1/2 value") to "spec §5.3 0-3 value".

### 2. `src/lib/Follow/FollowManager.cpp`

Replace `targetSane()` (`FollowManager.cpp:432-451`) with:

```cpp
bool FollowManager::targetTooFar(const FollowTarget &target) const
{
    GNSSLocation targetLoc{};
    targetLoc.lat = (double)target.lat_1e7 / 1e7;
    targetLoc.lon = (double)target.lon_1e7 / 1e7;
    double distFromSelf = GNSSManager::getSingleton()->horizontalDistanceTo(targetLoc);
    return distFromSelf > config.maxTargetDistM;
}
```

In `loop()` (`FollowManager.cpp:556-573`), replace the condition-code assembly and the `targetSane()` call:

```cpp
// spec §3.2: sequential, single-value condition code — raise to whichever
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
    updateStatusGvars(conditionCode);
    return;
}

if (targetTooFar(target))
{
    raiseCondition(FOLLOW_CONDITION_TARGET_TOO_FAR);
    updateStatusGvars(conditionCode);
    return;
}
```

The other two call sites that assign `conditionCode` directly (`FollowManager.cpp:499`, gate-inactive branch, and
the `resolveLock() == nullptr` early return) are unaffected — they only ever choose between `NONE` and
`RC_INVALID_GAP_SETTINGS` (pre-arm check), neither of which changes value or meaning here.

Update the comment at `FollowManager.cpp:700` (`// spec §5.3's 0/1/2 table...`) to reflect the new 0-3 range.

### 3. `docs/user-guide-follow-mode.md`

- Add a short callout above §6.1's table noting the renumber and that pilots with an existing Logic Condition on
  `conditionFlagsGvarIndex == 2` need to update it to `3`.
- Update the Condition Flags table (`user-guide-follow-mode.md:265-269`):

  | GVAR value (Condition Flags) | Meaning |
  |---|---|
  | `0` | No condition active |
  | `1` | Altitude floor is actively clamping the commanded altitude |
  | `2` | Target too far from this craft — Follow paused rather than chasing across an unbounded distance (see Max Target Distance, §4) |
  | `3` | RC-driven slot is frozen at its last safe position, or the pre-arm RC check failed (§7) |

- §6.2's CLI example currently defines exactly one `osd_custom_elements` line for the condition GVAR, gated on
  "nonzero" (visibility type `1`) and hardcoded to the text `"ALT FLOOR"` — that was already a slight
  oversimplification once code `2` (RC) shipped, and would now mislabel all three conditions identically. Replace
  it with three distinct Logic-Condition-gated elements, mirroring the status block's pattern:

  ```
  logic 4 1 -1 1 5 <CONDITION_FLAGS_GVAR_INDEX> 0 1 0
  logic 5 1 -1 1 5 <CONDITION_FLAGS_GVAR_INDEX> 0 2 0
  logic 6 1 -1 1 5 <CONDITION_FLAGS_GVAR_INDEX> 0 3 0
  osd_custom_elements 4 1 0 0 0 0 0 2 4 "ALT FLOOR"
  osd_custom_elements 5 1 0 0 0 0 0 2 5 "TOO FAR"
  osd_custom_elements 6 1 0 0 0 0 0 2 6 "BAD RC"
  ```

  Verify the exact `logic`/`osd_custom_elements` parameter values against a real INAV CLI session (bench test,
  item 3 below) before publishing — the existing four `logic` lines in the doc are the template to follow, but the
  new ones haven't been bench-verified yet.

### 4. `html/follow.js`

Update the `conditionFlagsGvarIndex` setting's `tip` text (`follow.js:303`), currently describing only the
altitude-floor case, to mention all three non-zero codes (or at least stop implying there's only one).

### 5. No changes needed

- `.claude/skills/web-ui-preview/mock_server.py` — `conditionFlagsGvarValue` is a hardcoded status readout, not
  derived from config; no new config field is added by this plan, so `DEFAULT_CONFIG`/`validate_config()` stay in
  sync without edits.
- `configJson()`/`applyConfig()`/EEPROM record — `maxTargetDistM` already exists as a persisted, validated field;
  this plan only changes which condition code its violation reports, not the field itself.
- `statusJson()` — already reports `conditionFlagsGvarValue` generically; no changes needed for a new enum value.

## Testing (bench)

1. **Regression:** with `maxTargetDistM` at its default and a normal-distance leader, confirm the condition GVAR
   still reports `0`/`1`/`3` exactly as before in the floor-clamp and RC-frozen cases (values shifted from `2`→`3`
   for the RC case — update any existing bench-test expectations accordingly).
2. **New condition:** set `maxTargetDistM` artificially low (e.g. `5`m) with a peer positioned farther away than
   that; confirm the condition GVAR reports `2`, the waypoint stops updating (`lastTarget` stops advancing in
   `GET /followmanager/status`), and it reverts to `0` once the peer is back within range.
3. **Priority ordering:** contrive a cycle where both the too-far condition and an RC-invalid-gap condition are
   simultaneously true (e.g. RC pushed to an unsafe extreme while the peer is also out of `maxTargetDistM` range);
   confirm the GVAR reports `3` (RC wins), not `2`. Then contrive floor-clamped + too-far simultaneously; confirm
   it reports `2` (too-far wins over floor).
4. **INAV OSD wiring:** paste the updated §6.2 CLI block into a real INAV Configurator CLI session, verify all
   three `osd_custom_elements` lines parse and each one's text appears/disappears correctly as the corresponding
   condition is triggered on the bench.

## Files touched

| File | Change |
|---|---|
| `src/lib/Follow/FollowManager.h` | Reorder/extend `FollowConditionCode`; replace `targetSane()` decl with `targetTooFar()` |
| `src/lib/Follow/FollowManager.cpp` | Replace `targetSane()` impl with `targetTooFar()`; rewrite `loop()`'s condition-code assembly to use `raiseCondition()`; comment updates |
| `docs/user-guide-follow-mode.md` | Renumber/extend condition-flags table; add breaking-change callout; update §6.2 CLI example to three distinct elements |
| `html/follow.js` | Update `conditionFlagsGvarIndex` tooltip text |
