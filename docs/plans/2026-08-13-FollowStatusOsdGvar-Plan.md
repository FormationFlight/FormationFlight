# FormationFlight — Follow-Mode Status on the Pilot's OSD (GVAR) — Implementation Plan

**Spec:** [`docs/spec/2026-08-13-FollowStatusOsdGvar.md`](../spec/2026-08-13-FollowStatusOsdGvar.md)
**Depends on:** [`2026-07-31-FollowMeOnInav-Plan.md`](2026-07-31-FollowMeOnInav-Plan.md) (all phases already shipped — `FollowRuntimeConfig`, `/followmanager/config`, EEPROM persistence, `html/follow.js` all exist today)
**Status:** Draft for review

Unlike the parent Follow-Me plan, none of this feature's supporting infrastructure needs to be built from scratch — `FollowRuntimeConfig`, its GET/POST endpoint, EEPROM persistence, and the `follow.js` panel are all already shipped and working. This addendum only adds two GVAR index fields to structures that are already wired end-to-end, plus one new MSP write path. Per discussion, this plan is one cohesive implementation phase (not a multi-phase DAG) followed by a dedicated bench-test pass.

---

## Correction against source (found during planning, not yet in the spec)

Spec §2.1 states GVARs are "signed 16-bit integers (`-32768`..`32767`)". Verified against INAV's actual firmware source (`iNavFlight/inav`, `master` branch, fetched during planning):

- `src/main/fc/fc_msp.c:2408-2422` — the `MSP2_INAV_SET_GVAR` handler requires `dataSize == 5` and reads `uint8_t gvarIndex` followed by `int32_t gvarValue` (`sbufReadU32`, cast to `int32_t`). **The payload is 5 bytes: a 1-byte index + a 4-byte signed value, not a 2-byte value.**
- `src/main/programming/global_variables.h` — `gvSet(uint8_t index, int32_t value)`; GVARs are `int32_t` internally, not `int16_t`. `#define MAX_GLOBAL_VARIABLES 8` confirms the spec's "INAV supports exactly 8 GVARs, indices 0-7" claim.

This doesn't change any design decision in the spec — every value FF ever writes here (0-4, or 0/1 for the condition code) fits trivially in either width — but it does change the wire struct FF must send. Work item 1 below uses the correct 5-byte layout. Worth folding into the spec's §2.1 text at some point, but not blocking this plan.

**Also carried in from the spec's revision note:** the second GVAR is named `conditionFlagsGvarIndex`, not `altitudeFloorGvarIndex` — see spec §3.2. This iteration still only implements one condition (altitude-floor clamp, code `1`); the rename just stops the field/GVAR from being hard-scoped to that one meaning. All identifiers below use the new name.

---

## Decisions carried in from clarifying questions

- **EEPROM version bump (2→3):** accepted as a plain reset-to-compile-defaults for anyone who already saved Follow config to EEPROM on a prior build — consistent with how `FollowManager::loadFromEEPROM()` already treats any version mismatch (`FollowManager.cpp:560-580`). No migration code. Worth one line in release notes when this ships.
- **"ID LOST" (GVAR value `4`) representation:** derived, not a new `PeerLock` state. `resolveLock()` (`FollowManager.cpp:83-159`) already leaves a distinguishing signal on the id-reuse-mismatch path: it clears `lockedId` to `0` but leaves `state` at `FOLLOW_LOCK_LOCKED_HOLDING` (`FollowManager.cpp:156`, no `state = ...` line follows), and `lockedId` can only be `0` while `state == FOLLOW_LOCK_LOCKED_HOLDING` via that one code path — entering `LOCKED_HOLDING` from `LOCKED` always starts with a nonzero `lockedId` (`FollowManager.cpp:129-136`). So `(state == FOLLOW_LOCK_LOCKED_HOLDING && lockedId == 0)` unambiguously means "id was invalidated," with no changes to the existing, already-shipped state machine.
- **Plan structure:** one implementation phase, then a bench-test phase.

---

## Design summary

Three new pieces, wired into existing infrastructure:

1. **MSP wire layer** (`MSP.h`, `MSPManager.{h,cpp}`) — `MSP2_INAV_SET_GVAR` command id, `msp_set_gvar_t` struct, `MSPManager::sendGvar()`, self-gated on INAV + version ≥ 9 (spec §2.2 Option A, reusing the already-cached `getFCVersion()`/`getFCVariant()`).
2. **FollowManager status derivation + emission** — a pure function mapping `(state, lockedId)` → the §3 status code, plus a secondary condition code captured where the altitude-floor clamp decision already happens in `loop()` (spec §3.2 — this is the only condition implemented, but the GVAR/field itself is named generically, `conditionFlagsGvarIndex`, so future non-exclusive conditions can reuse the same slot as additional codes rather than needing another rename). Both are sent through a small helper that only writes when the value changed or a heartbeat interval has elapsed (spec §3.3), inserted at `loop()`'s existing exit points so gate-inactive/startup correctly writes `0` (spec §3.1).
3. **Runtime config / EEPROM / web UI** — two more `int16_t` fields riding along on the existing `FollowRuntimeConfig` / `FollowEepromRecord` / `/followmanager/config` / `follow.js` machinery, exactly like every other §9-style key already does.

No new files, no new endpoints, no `targets/*.ini` changes (both new fields default to `-1`/disabled, so no target needs to seed them to fly as before).

---

## Work items

### 1. MSP wire layer

**`src/lib/MSP/MSP.h`**
- Near the other MSP2 defines (`MSP.h:79-84`): add `#define MSP2_INAV_SET_GVAR 0x2214 // SET a Global Variable's value (INAV 9.0+)`.
- Near `msp_set_wp_t` (`MSP.h:668-680`): add
  ```cpp
  // MSP2_INAV_SET_GVAR command payload (INAV 9.0+). index is 0-7 (INAV
  // supports MAX_GLOBAL_VARIABLES == 8, verified against inav/src/main/
  // programming/global_variables.h); value is INAV's native int32
  // (verified against inav/src/main/fc/fc_msp.c's MSP2_INAV_SET_GVAR
  // handler, which requires exactly a 5-byte payload — not 2 bytes, despite
  // the parent spec's §2.1 text, corrected here).
  struct msp_set_gvar_t {
    uint8_t index;
    int32_t value;
  } __attribute__((packed));
  ```

**`src/lib/MSP/MSPManager.h`**
- Add to the public interface, near `sendFollowWaypoint`:
  ```cpp
  // Writes a single INAV Global Variable over MSP2_INAV_SET_GVAR (spec
  // docs/spec/2026-08-13-FollowStatusOsdGvar.md). One-way, best-effort, no
  // ACK wait. Silently no-ops if the connected FC isn't INAV 9.0+ (§2.2) —
  // callers don't need to check support themselves.
  void sendGvar(uint8_t index, int32_t value);
  ```

**`src/lib/MSP/MSPManager.cpp`**
- Implementation, next to `sendFollowWaypoint` (`MSPManager.cpp:273-284`):
  ```cpp
  void MSPManager::sendGvar(uint8_t index, int32_t value)
  {
      if (!ready || getFCVariant() != HOST_INAV)
      {
          return;
      }
      // getFCVersion() is already cached for the connection's lifetime
      // (used today by Display.cpp's FC-version readout) — this adds no
      // extra MSP traffic to check support (spec §2.2 Option A).
      if (getFCVersion().versionMajor < 9)
      {
          return;
      }

      msp_set_gvar_t g{};
      g.index = index;
      g.value = value;
      msp->command2(MSP2_INAV_SET_GVAR, &g, sizeof(g), 0); // fire-and-forget, mirrors sendRadar()
  }
  ```
- Matches the existing `getState()`/`isGCSNavActive()` self-gating pattern (`MSPManager.cpp:29-47`) rather than `sendFollowWaypoint()`'s no-gate style — appropriate here since, unlike the core waypoint stream, this is a genuinely optional/version-dependent capability the caller shouldn't have to know about.

*Test (bench):* call `sendGvar(0, 42)` manually against a bench INAV 9.x FC, confirm GVAR 0 reads `42` in INAV Configurator's Programming tab. Repeat against an INAV < 9.0 FC (or a non-INAV host) and confirm no MSP traffic is sent (log `ready`/`getFCVariant()`/`getFCVersion()` around the call) and nothing hangs.

---

### 2. FollowManager: status derivation and emission

**`src/lib/Follow/FollowConfig.h`** — two new compile-time defaults, following the exact `#ifndef` pattern every other key already uses:
```cpp
// GVAR index for the primary lock-state indicator (spec §3), or -1 to
// disable (default — zero MSP traffic, zero OSD dependency until a pilot
// opts in via the web UI). INAV supports indices 0-7.
#ifndef FOLLOW_STATUS_GVAR_INDEX
#define FOLLOW_STATUS_GVAR_INDEX -1
#endif

// GVAR index for the secondary condition-code indicator (spec §3.2), or -1
// to disable. Independent of FOLLOW_STATUS_GVAR_INDEX — a pilot can enable
// either, both, or neither. Deliberately generic: only the altitude-floor
// clamp (code 1) is implemented today, but the field/GVAR isn't scoped to
// that one condition — future non-exclusive conditions get code 2, 3, etc.
// on this same slot without another rename.
#ifndef FOLLOW_CONDITION_FLAGS_GVAR_INDEX
#define FOLLOW_CONDITION_FLAGS_GVAR_INDEX -1
#endif
```

**`src/lib/Follow/FollowManager.h`**
- `FollowRuntimeConfig`: add
  ```cpp
  // GVAR indices (spec §3.4), -1 = disabled. Range enforced in
  // applyConfig() (-1 or 0-7); the web UI additionally makes an
  // out-of-range value structurally unreachable via a <select>.
  int16_t statusGvarIndex = FOLLOW_STATUS_GVAR_INDEX;
  int16_t conditionFlagsGvarIndex = FOLLOW_CONDITION_FLAGS_GVAR_INDEX;
  ```
- `FollowEepromRecord`: add the same two `int16_t` fields (no rounding needed going either direction, unlike the `double` geometry fields — these are already integers). Bump `#define FOLLOW_EEPROM_VERSION 2` to `3`.
- Private state, next to the existing `lastEepromCommitMs`/`nextRunTime` fields:
  ```cpp
  // Last value actually written to each GVAR (spec §3.3's change+heartbeat
  // send rule), or INT32_MIN as a "never sent yet" sentinel so the very
  // first loop() cycle always sends — this is what satisfies spec §3.1's
  // "write 0 explicitly at startup," with no separate startup-only code path.
  int32_t lastSentStatusGvarValue = INT32_MIN;
  int32_t lastSentConditionFlagsGvarValue = INT32_MIN;
  unsigned long lastStatusGvarSendMs = 0;
  unsigned long lastConditionFlagsGvarSendMs = 0;
  ```
- Private method:
  ```cpp
  // Derives this cycle's GVAR values from current state and sends whichever
  // of the two configured GVARs (spec §3.4) changed or are due for their
  // heartbeat resend (spec §3.3). floorClamped is the only condition
  // updateStatusGvars() knows how to derive a conditionFlagsGvarIndex code
  // from today (spec §3.2 — code 1); it's only meaningful when a waypoint
  // was actually computed this cycle (peer resolved) — pass false from
  // every other call site (spec §3.2's "write 0 on gate-inactive too"). A
  // future second condition would extend this function's mapping, not its
  // signature's meaning.
  void updateStatusGvars(bool floorClamped);
  ```

**`src/lib/Follow/FollowManager.cpp`**
- Near the top, alongside the existing `#define`s (`FollowManager.cpp:8-21`):
  ```cpp
  // How often to resend a GVAR even if its value hasn't changed, so a
  // single dropped MSP write doesn't leave the OSD showing a stale state
  // indefinitely (spec §3.3). 20x less frequent than the default 4 Hz
  // waypoint stream — negligible added MSP traffic (spec §8's "don't flood
  // MSP" budget, referenced by §3.3).
  #define FOLLOW_GVAR_HEARTBEAT_MS 5000
  ```
- New free function (mirrors `lockStateName()`'s placement/style, `FollowManager.cpp:367-377`):
  ```cpp
  // Spec §3's status code. IDLE only appears transiently (loop() sets it
  // right before the gate-inactive early return, where floorClamped is
  // always passed as false) — included for completeness, not reachable
  // with a nonzero code.
  static int32_t statusGvarValue(FollowLockState state, uint8_t lockedId)
  {
      switch (state)
      {
          case FOLLOW_LOCK_ACQUIRING:      return 1;
          case FOLLOW_LOCK_LOCKED:         return 2;
          // lockedId == 0 only happens here via the id-reuse-mismatch path
          // in resolveLock() (spec §6.3 caveat of the parent spec) — see
          // this plan's "ID LOST" decision above.
          case FOLLOW_LOCK_LOCKED_HOLDING: return lockedId == 0 ? 4 : 3;
          case FOLLOW_LOCK_IDLE:
          default:                          return 0;
      }
  }
  ```
- `updateStatusGvars()` implementation:
  ```cpp
  void FollowManager::updateStatusGvars(bool floorClamped)
  {
      MSPManager *msp = MSPManager::getSingleton();
      unsigned long now = millis();

      if (config.statusGvarIndex >= 0)
      {
          int32_t value = statusGvarValue(state, lockedId);
          bool due = lastSentStatusGvarValue == INT32_MIN
                   || value != lastSentStatusGvarValue
                   || (now - lastStatusGvarSendMs) >= FOLLOW_GVAR_HEARTBEAT_MS;
          if (due)
          {
              msp->sendGvar((uint8_t)config.statusGvarIndex, value);
              lastSentStatusGvarValue = value;
              lastStatusGvarSendMs = now;
          }
      }

      if (config.conditionFlagsGvarIndex >= 0)
      {
          // Only one condition exists today (altitude-floor clamp, spec
          // §3.2 code 1); a future second condition adds another branch
          // here, not another GVAR.
          int32_t value = floorClamped ? 1 : 0;
          bool due = lastSentConditionFlagsGvarValue == INT32_MIN
                   || value != lastSentConditionFlagsGvarValue
                   || (now - lastConditionFlagsGvarSendMs) >= FOLLOW_GVAR_HEARTBEAT_MS;
          if (due)
          {
              msp->sendGvar((uint8_t)config.conditionFlagsGvarIndex, value);
              lastSentConditionFlagsGvarValue = value;
              lastConditionFlagsGvarSendMs = now;
          }
      }
  }
  ```
- `loop()` (`FollowManager.cpp:293-365`) — four small, surgical insertions at its existing exit points, no restructuring:
  1. Gate-inactive branch (`FollowManager.cpp:306-312`): add `updateStatusGvars(false);` right before the `return;` (state has just been set to `FOLLOW_LOCK_IDLE`, so this writes `0`/`0` — satisfies spec §3.1's explicit-zero requirement, including at startup via the `INT32_MIN` sentinel).
  2. `resolveLock()` returns `nullptr` (`FollowManager.cpp:314-318`, ACQUIRING or LOCKED_HOLDING/ID-LOST): add `updateStatusGvars(false);` before the `return;` — no waypoint is being computed this cycle, so `conditionFlagsGvarIndex` reports `0`.
  3. Capture the clamp decision where it already happens (`FollowManager.cpp:343-347`):
     ```cpp
     int32_t floorCm = (int32_t)lround(config.minAltM * 100.0);
     bool floorClamped = altCm < floorCm;
     if (floorClamped)
     {
         altCm = floorCm;
     }
     ```
  4. `targetSane()` rejects (`FollowManager.cpp:349-352`): add `updateStatusGvars(floorClamped);` before the `return;` — `state` is still `LOCKED` here (a sanity reject is a per-cycle skip, not a lock-state transition), so this correctly still reports `2`.
  5. End of a successful cycle (`FollowManager.cpp:359-365`, after `sendFollowWaypoint`): add `updateStatusGvars(floorClamped);`.
- `configJson()` (`FollowManager.cpp:418-442`): add
  ```cpp
  (*doc)["statusGvarIndex"] = config.statusGvarIndex;
  (*doc)["conditionFlagsGvarIndex"] = config.conditionFlagsGvarIndex;
  ```
- `applyConfig()` (`FollowManager.cpp:444` onward): add range validation alongside the existing field-sanity checks (same style as every other field — this is ordinary "is this a legal value" validation, distinct from the front-end-only collision guard, which spec §3.4 explicitly keeps client-side only):
  ```cpp
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
  ```
- `toEepromRecord()`/`fromEepromRecord()` (`FollowManager.cpp:505-558`): add straight-through copies (`record.statusGvarIndex = config.statusGvarIndex;` etc. — `int16_t` both sides, no rounding, unlike the `double` geometry fields).
- `statusJson()` (`FollowManager.cpp:379-393`) — debug aid for the bench-test pass, so a tester can confirm the exact value being written without opening INAV Configurator (mirrors Phase 2's original rationale in the parent plan):
  ```cpp
  if (config.statusGvarIndex >= 0 && lastSentStatusGvarValue != INT32_MIN)
  {
      (*doc)["statusGvarValue"] = lastSentStatusGvarValue;
  }
  if (config.conditionFlagsGvarIndex >= 0 && lastSentConditionFlagsGvarValue != INT32_MIN)
  {
      (*doc)["conditionFlagsGvarValue"] = lastSentConditionFlagsGvarValue;
  }
  ```

---

### 3. Web UI

**`src/lib/WiFi/WiFiManager.cpp`** — `handleFollowManagerConfigPost()` (`WiFiManager.cpp:301-351`): add, alongside the other `hasParam`/`toInt()` lines:
```cpp
if (request->hasParam("statusGvarIndex", true)) cfg.statusGvarIndex = (int16_t)strParam("statusGvarIndex").toInt();
if (request->hasParam("conditionFlagsGvarIndex", true)) cfg.conditionFlagsGvarIndex = (int16_t)strParam("conditionFlagsGvarIndex").toInt();
```
No new endpoint — same GET/POST `/followmanager/config` pair every other field already goes through.

**`html/follow.js`**
- New options array, next to `headingModeOptions`:
  ```js
  const gvarIndexOptions = [[-1, 'Disabled']].concat([0,1,2,3,4,5,6,7].map(i => [i, String(i)]));
  ```
- Extend `validateConfig()` with the front-end-only collision guard (spec §3.4 — no server-side equivalent):
  ```js
  if (cfg.statusGvarIndex !== -1 && cfg.statusGvarIndex === cfg.conditionFlagsGvarIndex) {
    return 'Status and Condition Flags GVAR indices must be different (or both Disabled)';
  }
  ```
- New panel (or folded into the existing "Trigger & Target" panel — implementer's call, both are one `Setting` block each so it's a small either way), with the required "requires INAV 9.0+" note (spec §3.4, surfaced as a static note, not a live-connection gate):
  ```js
  <div class="py-1 divide-y border rounded bg-white flex flex-col">
    <div class="font-light uppercase flex items-center text-gray-600 px-4 py-2">
      OSD Status (GVAR)
    <//>
    <div class="py-2 px-5 flex-1 flex flex-col relative">
      <div class="text-xs text-gray-500 mb-2">Requires INAV 9.0 or later on the follower FC. Values are written but ignored on older firmware.</div>
      <${Setting} title="Status GVAR Index" tip="Which INAV Global Variable to write the follow lock-state code to (0=inactive, 1=searching, 2=locked, 3=holding, 4=id lost). Configure a matching Custom OSD element in INAV Configurator to display it." value=${config.statusGvarIndex} setfn=${mksetfn('statusGvarIndex')} type="select" options=${gvarIndexOptions} />
      <${Setting} title="Condition Flags GVAR Index" tip="Which INAV Global Variable to write a secondary condition code to. Currently the only condition is the altitude floor actively clamping the commanded altitude (0=no condition, 1=altitude floor clamped); more conditions may be added to this same slot in the future." value=${config.conditionFlagsGvarIndex} setfn=${mksetfn('conditionFlagsGvarIndex')} type="select" options=${gvarIndexOptions} />
      ${status.statusGvarValue !== undefined && html`<div class="text-xs text-gray-500 mt-1">Last written: status=${status.statusGvarValue}${status.conditionFlagsGvarValue !== undefined ? `, condition=${status.conditionFlagsGvarValue}` : ''}<//>`}
    <//>
  <//>
  ```
- `applyLive()`'s POST body: add
  ```js
  body.append('statusGvarIndex', config.statusGvarIndex);
  body.append('conditionFlagsGvarIndex', config.conditionFlagsGvarIndex);
  ```

*Test (UI):* set both dropdowns to the same non-Disabled index, confirm Save is blocked client-side with the collision message; set them to different indices (or one Disabled), confirm it saves; confirm the "Last written" debug line updates as follow state changes during a bench run.

---

## Bench-test pass (spec §12-style acceptance, scoped to this addendum)

Requires: an INAV 9.0+ bench FC (for the positive path) and, ideally, a pre-9.0 or non-INAV MSP host (for the negative/no-op path) — the parent plan's `/peermanager/spoof` tooling (Phase 0E) covers driving the peer-lock state machine through its states without a real second aircraft.

1. **Version gating (negative path):** with a non-INAV or INAV < 9.0 bench FC connected, enable both GVAR indices via the UI, drive the follow state through a few transitions, confirm via a serial/log tap (or simply the absence of any MSP2_INAV_SET_GVAR traffic on a bus analyzer/logic probe if available) that `sendGvar()` never actually writes — and that nothing else in FF misbehaves as a result (no hang, no repeated retry).
2. **Wire format (positive path):** with an INAV 9.0+ bench FC, set `statusGvarIndex` to e.g. `0` via the UI, engage the follow gate with no peer spoofed yet, confirm GVAR 0 reads `1` (ACQUIRING) in INAV Configurator's Programming tab; spoof a fresh peer and confirm it reads `2` (LOCKED).
3. **Explicit-zero at gate-inactive/startup (spec §3.1):** with the gate inactive, confirm GVAR 0 reads `0` (not stale from a previous run). Reboot the bench unit mid-"LOCKED" (if feasible) and confirm the GVAR reads `0` shortly after boot, before any peer is available — not a leftover `2`.
4. **LOCKED_HOLDING vs. ID LOST distinction:** spoof a peer, lock it, then stop its spoofed updates past `peerTimeoutMs` — confirm GVAR reads `3`. Separately, force the id-reuse-mismatch path (spoof a peer, lock it, let it go stale into `LOCKED_HOLDING`, then respawn a *different* spoofed peer under the same id with a different name) — confirm GVAR reads `4`, and that it's only recoverable via a follow-gate switch cycle, not by the original peer's telemetry resuming.
5. **Altitude-floor condition code:** spoof a leader descending toward/below the follower's altitude floor (reusing the parent plan's still-outstanding §12.1 item 10 test setup) with `conditionFlagsGvarIndex` enabled; confirm the GVAR flips to `1` exactly while the clamp is active and back to `0` once it isn't, and stays `0` throughout ACQUIRING/HOLDING (never stale-`1` from a prior LOCKED cycle).
6. **Heartbeat:** with the state held steady (e.g. LOCKED with a continuously-fresh spoofed peer), confirm via `/followmanager/status`'s `ageMs`-style timing or a bus tap that a GVAR write still recurs roughly every `FOLLOW_GVAR_HEARTBEAT_MS` (5s) even though the value never changes — and that it is *not* recurring at the 4 Hz waypoint-stream rate.
7. **Two independent GVARs:** confirm `statusGvarIndex`-only, `conditionFlagsGvarIndex`-only, both, and neither all behave correctly (no traffic when both are `-1`; each is independently toggleable).
8. **EEPROM persistence + version bump:** set both indices via the UI, "Save to EEPROM," reboot, confirm they survive. Separately (on a unit that saved Follow config under the old `FOLLOW_EEPROM_VERSION == 2` layout, if one is available from parent-plan testing), confirm it comes up on compile-time defaults rather than a garbled read — the accepted, documented consequence of the version bump above.
9. **INAV-side OSD wiring (documentation, spec §5):** once the above is confirmed working end-to-end, write up the Configurator-side Logic Condition + Custom OSD element setup (illustrative text from spec §3's table) as FF user-facing docs — this is the one item spec §6 left genuinely open. Include the §3.2 "combine two GVARs into one Custom OSD element's visibility" Logic Condition wiring (AND/OR the two GVAR values) as part of that writeup.

---

## Files touched

| File | Purpose |
|---|---|
| `src/lib/MSP/MSP.h` | `MSP2_INAV_SET_GVAR` define, `msp_set_gvar_t` struct |
| `src/lib/MSP/MSPManager.{h,cpp}` | `sendGvar()`, version/host-gated |
| `src/lib/Follow/FollowConfig.h` | `FOLLOW_STATUS_GVAR_INDEX` / `FOLLOW_CONDITION_FLAGS_GVAR_INDEX` defaults |
| `src/lib/Follow/FollowManager.{h,cpp}` | status derivation, `updateStatusGvars()`, config/EEPROM fields, `configJson()`/`statusJson()` additions, `applyConfig()` validation |
| `src/lib/WiFi/WiFiManager.cpp` | two new params on the existing `/followmanager/config` POST handler |
| `html/follow.js` | GVAR index dropdowns, collision validation, debug "last written" readout |

No changes to `src/main.cpp`, `src/lib/ConfigHandler.cpp`, or `targets/*.ini`.
