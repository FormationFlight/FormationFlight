# FormationFlight — Follow-Mode Status on the Pilot's OSD (INAV Global Variable) — Engineering Spec

**Status:** Draft — not yet planned or implemented
**Target firmware:** FormationFlight (ESP32/ESP8266, PlatformIO/C++)
**Depends on:** [`2026-07-31-FollowMeOnInav.md`](2026-07-31-FollowMeOnInav.md) — specifically the `PeerLock` state machine (§6.3 of that spec), which this feature reports on. This is an addendum, not a replacement.

**Revision note:** the second GVAR (§3.2) was originally scoped and named solely around the altitude-floor clamp (`altitudeFloorGvarIndex`). Renamed to `conditionFlagsGvarIndex` so future non-exclusive conditions (not yet defined) can reuse the same GVAR slot without another rename — this iteration still only implements one condition (altitude-floor clamp, code `1`). Despite the "Flags" in the name, the value is a sequential numeric code (0 = none, 1 = altitude-floor clamped, 2+ reserved), not a bitmask — only one condition is reportable at a time; see §3.2.

---

## 1. Purpose & Scope

### 1.1 Problem
Follow mode's live state (searching for a leader, locked and tracking, or holding after losing the leader) is currently only visible through FF's own web UI (`/followmanager/status`, read-only endpoint shipped in Phase 2). A pilot flying FPV goggles has no practical way to check that endpoint mid-flight — they aren't tethered to FF's access point while flying, and shouldn't need to be. The only feedback they get today is indirect: the aircraft either flies the formation slot correctly or it doesn't, with no way to tell *why* from the goggles.

### 1.2 Goal
Surface the follower's `PeerLock` state as short OSD text on the pilot's existing display, so the pilot can tell at a glance whether follow mode is searching, locked and tracking, or holding after a lost leader — without leaving the goggles or touching a phone/laptop. This spec drives that text via an INAV **Global Variable (GVAR)**, set from FF over MSP, combined with INAV's own **Programming Framework** (Logic Conditions + Custom OSD elements) to turn that value into visible text.

### 1.3 In scope
- Defining the set of follow-mode states worth surfacing to the pilot, and the numeric GVAR value assigned to each (§3).
- Establishing this as a one-way, best-effort status channel: FF → follower FC (GVAR) → INAV Programming Framework (Custom OSD element visibility) → OSD text. Nothing reads state back from the OSD or the GVAR.
- Documenting, at a guidance level, the INAV-side Custom OSD/Logic Condition setup a pilot needs so the GVAR value actually becomes visible text (§5) — not automating that setup.

### 1.4 Out of scope (this iteration)
- Any change to INAV firmware — this uses an existing, INAV-native MSP command and existing OSD/Programming Framework features, unmodified.
- Automating the INAV-side Configurator setup (Logic Conditions, Custom OSD element placement/text) from FF. The pilot configures this once in INAV Configurator, the same way they'd configure any other OSD element; FF only ever writes the GVAR value.
- Dynamic/interpolated OSD text (e.g. embedding the locked leader's name in the message). GVARs hold integers only, so each state's on-screen text is a fixed string chosen by the pilot at Configurator-setup time, not composed by FF at runtime. A generic state indicator (e.g. `LOCKED`) is sufficient — see §3.
- Leader-side or peer-radar-display changes — this is about the *follower's own* follow-mode state, not peer visibility (which FF already reports separately via its radar HUD feed).
- Rich/graphical OSD elements beyond what INAV's Custom OSD elements natively support (icons tied to a GVAR/Logic Condition are a native INAV capability, not something FF needs to build).
- Replacing or duplicating FF's own web UI status endpoint — that remains the detailed/authoritative source (full lock state, locked peer id/name, last target, age); the OSD text is a glance-only summary for use while flying.

---

## 2. Background

INAV's **Programming Framework** lets a pilot define Logic Conditions and Global Variables entirely within INAV (Configurator or CLI), and bind **Custom OSD elements** to them — each element shows fixed text/icons whose *visibility* is gated on a Logic Condition or GVAR value. This is a generic, purpose-built INAV feature, unrelated to any specific peripheral or vehicle type.

Since INAV 9.0.0 (released January 2025), INAV also exposes **`MSP2_INAV_SET_GVAR`** (MSP2 command `0x2214`), letting an external device — like FF — write a GVAR's value directly over MSP. Combined, this gives FF a fully INAV-native path to drive pilot-configured OSD text: FF writes an integer state code to a GVAR every time the follow state changes; the pilot has pre-configured one Custom OSD element per state, each visible only when the GVAR matches that state's value.

### 2.1 Wire contract (fixed by INAV, for reference)
- `MSP2_INAV_SET_GVAR` (`0x2214`) sets a single GVAR's value. GVARs are signed 16-bit integers (`-32768`..`32767`) — ample range for a small state enum.
- One-way and best-effort, same as the parent spec's waypoint stream: if a write isn't delivered a cycle, INAV simply keeps the GVAR (and thus the OSD text) at its last value. There's no ack/retry contract to design around.
- **Hard requirement: follower FC must run INAV 9.0.0 or later.** Older INAV has no `MSP2_INAV_SET_GVAR` handler; see §2.2 for how FF detects and handles this.

### 2.2 Version gating: how FF decides whether the FC supports this

Two candidate approaches were investigated:

**Option A — query `MSP_FC_VERSION` up front, gate on `versionMajor >= 9`.** FF already has this: `MSPManager::getFCVersion()` (`src/lib/MSP/MSPManager.cpp:108-118`) sends `MSP_FC_VERSION`, caches the parsed `msp_fc_version_t{versionMajor, versionMinor, versionPatchLevel}` on first successful response, and is already consumed elsewhere (`src/lib/Display/Display.cpp:343-368`, for the OLED's FC-version readout). Combined with the existing `MSPManager::getFCVariant() == HOST_INAV` check (`MSPManager.cpp:63-104`, already used by `isGCSNavActive()`/`getAnalogValues()` to gate INAV-only behavior), the feature would enable only once both `hostIsFlightController` reports `HOST_INAV` and `versionMajor >= 9`. No wire-format additions, no new parsing.

**Option B — infer support from how the FC responds to a real `MSP2_INAV_SET_GVAR` write** (an "unknown command" response implies pre-9.0.0). This turns out to be materially harder in FF's current MSP stack than it looks: `MSP::recv`/`MSP::recv2` (`src/lib/MSP/MSP.cpp:96-204`) only recognize the success-direction header (`$M>` / `$X>`); neither function has any code path for MSP's error-direction byte (`$M!` / `$X!`). An error response today is simply not matched by the header check, gets silently discarded, and the read loop keeps waiting — so `waitFor`/`waitFor2` (and thus `command`/`command2`) already return `false` for *both* "FC replied with an explicit error" and "FC never replied at all / the frame got dropped on a lossy link." Making Option B actually mean what it claims requires: (1) teaching the shared MSP parser to recognize and surface `!`-direction frames without breaking every other caller of `recv`/`recv2`, and (2) correlating that error to the specific request that caused it (not just "some error arrived"), on a best-effort link where dropped/garbled frames are expected and are not evidence of missing FC support. Getting that wrong means a single lost frame permanently (until reboot) disables the feature, misreported as "unsupported firmware."

**Recommendation: Option A.** It reuses infrastructure that already exists and is already trusted for a materially similar purpose (Display.cpp's version-gated readout), requires no changes to the shared MSP parser, and doesn't rely on outcome-correlating a best-effort link's error responses. Query once (result is cached for the connection's lifetime, same as today), gate the whole feature on `getFCVariant() == HOST_INAV && getFCVersion().versionMajor >= 9`. If the version query itself hasn't succeeded yet (FC not connected/booted), the feature simply stays quiescent until it has — consistent with how `isGCSNavActive()` and friends already behave before `ready` is true.

---

## 3. What Gets Shown

This is the core design question this spec answers: which follow-mode conditions are worth a pilot's attention mid-flight, and what GVAR value / illustrative OSD text represents each. Exact text is chosen by the pilot at Configurator-setup time (§5), so the wording below is illustrative, not final copy — only the *meaning* and *state set* are spec-level.

| Condition (maps to parent spec §6.3 `PeerLock` states, plus one follow-specific safety condition) | Pilot-relevant meaning | Illustrative GVAR value | Illustrative OSD text |
|---|---|---|---|
| Follow gate inactive (switch off / `GCS NAV` not engaged) | Follow mode isn't running — nothing to report | `0` | *(no element visible — see §3.1)* |
| `ACQUIRING` — gate just went active, no leader locked yet | FF is looking for a leader to lock onto | `1` | `FOLLOW: SEARCHING` |
| `LOCKED` — tracking a leader, telemetry fresh | Normal operation. GVARs hold integers only, so this cannot name *which* leader — see §1.4 | `2` | `FOLLOW: LOCKED` |
| `LOCKED_HOLDING` — leader telemetry went stale or was lost | Most safety-relevant state: reassures the pilot the aircraft is holding position (not flying blind, not chasing something new), while making clear it is *not* currently tracking | `3` | `FOLLOW: HOLD (LOST)` |
| Re-locked after `LOCKED_HOLDING` (same leader's telemetry resumed) | Tracking has resumed automatically | `2` | same as `LOCKED` above |
| Peer identity mismatch caught (parent spec §6.3 caveat) | The lock was invalidated for safety reasons, distinct from an ordinary stale-telemetry hold — the pilot should know a switch cycle is needed to resume | `4` | `FOLLOW: ID LOST` |
| Commanded altitude was clamped to the safety floor (parent spec §7.6) | Informational: the follower is intentionally not following the leader's/offset's raw vertical position because it would have gone below the configured floor | separate GVAR, condition code `1` — see §3.2 | secondary indicator |

### 3.1 Should "not engaged" show anything?
Decision: no Custom OSD element is visible when the follow gate is inactive (no state's visibility condition matches GVAR value `0`), so the OSD stays clean on flights where follow mode isn't used — but FF **explicitly writes `0`** rather than simply pausing writes, both when the gate goes inactive and once at startup/reconnect before the first real state is known. This is deliberate: if FF only wrote on state *changes*, a reboot mid-flight (follow previously engaged, GVAR left at e.g. `2`/`LOCKED`) would leave INAV showing a stale, misleading "locked" indicator with no FF process alive to correct it. Writing `0` up front closes that gap — the OSD can treat `0` as an explicit, meaningful "not active" state rather than inferring it from silence.

### 3.2 Non-exclusive conditions need a second GVAR
Unlike the primary lock states (mutually exclusive by construction), some conditions are *modifiers* that can co-occur with a primary state (e.g. `LOCKED` *and* altitude-clamped at once). A single GVAR can only hold one value at a time, so representing this alongside the primary state cleanly requires a **second, independent GVAR** — gated and configured exactly like the primary status GVAR (see §3.4: its own user-assigned index, its own `-1` default/disable). This GVAR, `conditionFlagsGvarIndex`, is deliberately generic rather than scoped to one condition: it carries a small sequential numeric code (`0` = none, `1` = altitude-floor clamped) so a future non-exclusive condition can be added as code `2`, `3`, etc. without introducing a third GVAR or renaming this one. This iteration implements exactly one condition (altitude-floor clamp, parent spec §7.6, code `1`); only one condition is reportable at a time (a sequential code, not a bitmask — see revision note above), which is fine today since there is only one, but is a real limit to keep in mind if a second concurrent condition is added later (whichever comes second would need its own scheme, e.g. promoting this to a bitmask, at that point — not solved here).

A Custom OSD element's visibility binds to a single Logic Condition, so combining the two GVARs on-screen means routing them through an INAV Logic Condition that AND/ORs both values before a Custom OSD element reads it — not a native "multi-GVAR visibility" field. This keeps the two GVARs orthogonal rather than trying to encode both into one value's numbering scheme, and lets a pilot enable one without the other (e.g. lock-state only, no condition indicator). The same rule as §3.1 applies: FF writes `0` on gate-inactive/startup, not just on transitions into a condition becoming active.

### 3.3 Update cadence
Not every follow-loop cycle needs to re-send the GVAR — unlike the position waypoint (which must stream continuously to be useful), the status value only needs to change when the *state* changes. Sending on state transitions (plus a slow idle heartbeat so a dropped write doesn't linger forever) is the intended behavior; exact cadence is an implementation-stage decision, beyond the requirement that it must not meaningfully add to the follow link's existing MSP traffic budget (parent spec §8: "do not flood MSP").

### 3.4 GVAR index is pilot-assigned, per-GVAR opt-in via `-1`

Both GVARs this spec uses are indices the *pilot* picks, not fixed constants FF hardcodes — different aircraft, or a pilot already using GVARs for something else, may need different slots. This is exposed as two independent runtime-config fields, following the existing `FollowRuntimeConfig` pattern (`src/lib/Follow/FollowManager.h:36-62`):

- `statusGvarIndex` (`int16_t`, default `-1`) — GVAR index for the primary lock-state value (§3's main table).
- `conditionFlagsGvarIndex` (`int16_t`, default `-1`) — GVAR index for the secondary condition code (§3.2). Currently only ever `0` or `1` (altitude-floor clamp), but the field and its name aren't scoped to that one condition — see §3.2's revision note.

**`-1` means disabled, and is the default for both.** FF sends nothing for a GVAR whose index is `-1` — no MSP traffic, no OSD dependency, zero behavior change for a pilot who doesn't opt in. Setting either field to a non-negative value enables that GVAR's writes, gated on §2.2's version/host check. The two fields are independent: a pilot can enable lock-state only, condition-flags only, both, or neither.

**Range: dropdown, not a free-entry number field.** INAV currently supports exactly 8 GVARs, indices `0`-`7` (fixed by INAV itself, not an FF-chosen limit). Rather than a numeric input a pilot could mistype out of range, both fields are presented in `html/follow.js` as a `<select>` with 9 options: `Disabled` (value `-1`) followed by `0` through `7`. This makes an out-of-range index structurally impossible from the UI, so no separate range-validation open question remains (§6).

**Collision guard is front-end only.** If a pilot picks the same non-`-1` index for both dropdowns, the two GVARs would silently overwrite each other on the FC. `html/follow.js` checks this client-side on change/submit and blocks the conflicting selection with a warning (e.g. disable/flag the option or refuse to submit) — no corresponding check is added on the firmware side (`WiFiManager.cpp`'s `POST /followmanager/config` handler); this is a UX guard against a mistake, not a wire-level invariant FF enforces server-side. Two `Disabled` (`-1`) selections are never a collision.

**INAV-9.0+ requirement is surfaced as a UI note, not a UI gate.** The web UI displays a note next to both dropdowns stating this functionality requires INAV 9.0 or later (§2.2) — but does not prevent the pilot from selecting a GVAR index regardless of the connected FC's actual version. FF still enforces the real gate at the MSP layer (§2.2): on older/non-INAV firmware the fields can be set and saved, but no GVAR traffic is ever sent. This keeps the config UI decoupled from live FC-connection state (a pilot may configure ahead of a firmware upgrade, or configure with the FC disconnected).

Like the rest of `FollowRuntimeConfig`, both fields are:
- exposed as `<select>` dropdowns in the web UI (`html/follow.js`, same POST `/followmanager/config` path as the rest of §9's keys), and
- persisted in the EEPROM record (`FollowEepromRecord`, `FollowManager.h:88-109`) so the pilot's chosen indices survive a reboot — this requires bumping `FOLLOW_EEPROM_VERSION` (currently `2`) when the fields are added.

---

## 4. Non-Goals

- No attempt to make this message serve as the pilot's *only* safety indicator — the manual-override switch and INAV's own failsafe/RTH remain the authoritative safety mechanisms (mirrors parent spec §8's "FC mode is authoritative" framing). This is situational awareness, not a safety interlock.
- No dynamic/composed OSD text (e.g. leader name) — GVARs hold integers only, see §1.4.
- No configurability of *which* states get a message in this iteration (e.g. no per-state on/off toggles) — only whether each of the two GVARs (§3.4) is enabled.
- No multiple simultaneous condition codes on `conditionFlagsGvarIndex` in this iteration — it's a sequential numeric code, not a bitmask (§3.2's revision note), so only one condition can be reported at a time. Not a concern today (exactly one condition, altitude-floor clamp, is implemented) but a real limit for whoever adds the next one.
- No automation of the INAV-side Configurator setup — FF documents what's needed (§5); the pilot performs it in INAV Configurator like any other Custom OSD element.

---

## 5. INAV-Side Setup (guidance, not implementation)

For a GVAR's value to become visible text, the pilot must, once per aircraft, in INAV Configurator's Programming tab:
- Pick a free GVAR index for each of the one or two indicators wanted (lock-state, condition flags), and enter that same index into FF's web UI (§3.4) so both sides agree on the slot.
- Create one Custom OSD element per state in §3, each with the desired fixed text and a visibility condition matching that state's GVAR value (and, for the condition-flags GVAR — currently only ever the altitude-floor-clamp code — a Logic Condition combining it with the lock-state GVAR per §3.2, if a combined indicator is wanted).
- Position each element on the OSD layout as desired.

This spec calls out that this setup exists and roughly what it involves, so it can be referenced from FF's own setup docs; the exact instructions/screenshots are a documentation-stage task, not part of this engineering spec.

---

## 6. Open Questions

All questions raised during drafting have been resolved (see §3.4):
- **Index collision** (`statusGvarIndex` == `conditionFlagsGvarIndex`, both non-`-1`): resolved — front-end-only validation in `html/follow.js` blocks the conflicting selection; no firmware-side check.
- **Index range**: resolved — INAV supports 8 GVARs (`0`-`7`); the field is a dropdown (`Disabled`/`0`-`7`), not a free-entry number, so out-of-range values are structurally unreachable.
- **Quietly-disabled state on pre-9.0/non-INAV FCs**: resolved — the UI carries a static note that the feature requires INAV 9.0+, but does not gate or validate against the live connection; the pilot may set the fields regardless of connected FC state, and FF's MSP layer (§2.2) is the actual enforcement point.

One item remains genuinely open, deferred to the plan stage as pilot/Configurator-side documentation rather than an FF engineering decision:
- §3.2's exact INAV-side Logic Condition wiring for combining the two GVARs into one Custom OSD element's visibility condition (a dedicated "LOCKED + clamped" element vs. a small overlay icon) — noted here only so the plan stage remembers to document it in §5's setup guidance.
- Whether/when `conditionFlagsGvarIndex` gains a second condition code, and if so whether the sequential-code scheme (§3.2) still holds or needs to become a bitmask to represent concurrent conditions — deliberately deferred until there's a concrete second condition to design against, not resolved speculatively here.
