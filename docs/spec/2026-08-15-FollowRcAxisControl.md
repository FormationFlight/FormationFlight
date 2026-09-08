# FormationFlight — In-Flight RC Control of the Follow Slot — Engineering Spec

**Status:** Draft — not yet planned or implemented
**Target firmware:** FormationFlight (ESP32/ESP8266, PlatformIO/C++)
**Depends on:**
- [`2026-07-31-FollowMeOnInav.md`](2026-07-31-FollowMeOnInav.md) — the parent spec. This addendum reuses its canonical offset model (§7.3), geometry safety rules (§7.4), altitude floor (§7.6), and `FollowRuntimeConfig`/EEPROM persistence design (§10).
- [`2026-08-13-FollowStatusOsdGvar.md`](2026-08-13-FollowStatusOsdGvar.md) — the first addendum. This spec extends its `conditionFlagsGvarIndex` scheme (§3.2 of that spec) with a second condition code, exactly the extension point that spec's revision note anticipated.

---

## 1. Purpose & Scope

### 1.1 Problem
Today the follow slot (`ofsLongM`/`ofsLatM`/`ofsVertM`, parent spec §7.3) can only be changed by a pilot connected to FF's web UI — impossible while flying. A pilot who wants to nudge the formation slot in the air (widen a gap, drop below the leader to clear turbulence, tuck in tighter for a photo pass) currently has no way to do that without landing, connecting a phone/laptop to FF's AP, editing the panel, and taking off again.

### 1.2 Goal
Let a pilot trim the follow slot live, in flight, using up to three existing RC channels on their transmitter (one per axis), read from the flight controller over MSP the same way FF already reads other FC state. Each assigned channel scales its axis between the *negative* and *positive* of that axis's currently configured gap — the configured value becomes a **bound**, not a fixed point, once RC control is enabled for that axis.

### 1.3 In scope
- Mapping a 1000–2000µs RC channel value to a signed offset in `[-|configured gap|, +|configured gap|]`, independently per axis (§3).
- Three new runtime-config fields (one RC channel assignment per axis), live-editable and EEPROM-persisted the same way every other `FollowRuntimeConfig` field already is (parent spec §10) (§7).
- A live, every-cycle re-validation of the RC-scaled offset triple against the existing minimum-separation geometry rules (parent spec §7.4), with a defined fallback when RC input would produce an unsafe slot (§4).
- A new `conditionFlagsGvarIndex` code, `2` = "Incorrect Setting" (per the OSD/GVAR spec's §3.2 extension point), covering both the geometry fallback above and a specific altitude-floor interaction (§5, §6).
- Reading RC channel values from the FC over MSP (`MSP_RC`), a new integration point not previously used anywhere in FF (§2).
- A pre-arm, ground-only advisory check that warns (via the same condition code, plus a status-endpoint field) when the pilot's current RC input either disagrees with the safety logic in a way that would sign-lock or freeze the slot the moment follow engages, **or** simply doesn't reproduce the configured static default on every RC-assigned axis — RC-driven offsets are only meant to take effect once airborne (§4.6).

### 1.4 Out of scope (this iteration)
- Any change to the *base* configured offset (`ofsLongM`/`ofsLatM`/`ofsVertM`) from RC input. RC never writes back to the stored config or EEPROM — it only modifies what `FollowManager` computes and emits this cycle. Landing and rebooting always returns to the last-saved configured slot, never to wherever RC happened to leave it (mirrors parent spec §10.1's "live edits are session-only until explicitly committed" framing, extended to RC's cycle-by-cycle scaling).
- Smoothing/filtering of the RC-derived value (e.g. exponential/rate-limited slewing). The mapping is direct and stateless every cycle — see §8's non-goal note.
- Auto-detecting or labeling channels by function (e.g. "AUX1", "this is your throttle"). The pilot picks a channel number knowing their own transmitter/receiver mapping, the same way they would configure any other AUX-driven INAV feature.
- A dedicated FF-side RC receiver input. FF reads channel values already decoded by the FC (§2.1).
- Any interaction with the follow-mode trigger/gate itself (`FOLLOW_TRIGGER_MODE`, parent spec §5[C]) or the manual-override switch. RC axis control only ever adjusts *where* the slot is while follow mode is already active — it cannot engage or disengage following, and the manual-override switch remains the authoritative way to hand control back to the pilot (parent spec §8).

### 1.5 Assumptions
- The follower FC is INAV, connected over MSP, the same link FF already uses for every other follow-mode data source (parent spec §5).
- The pilot's transmitter/receiver already delivers the channel(s) they want to assign through to the FC in the normal way (SBUS/CRSF/etc.) — this spec only reads whatever the FC reports via `MSP_RC`, exactly as INAV Configurator's own channel monitor would.

---

## 2. Background

### 2.1 Reading RC channels: `MSP_RC`, not a new receiver input
FF has no existing RC-channel-reading code — `MSP.h` already defines the wire struct (`msp_rc_t`, `channelValue[MSP_MAX_SUPPORTED_CHANNELS]`, `MSP.h:263-264`; `MSP_MAX_SUPPORTED_CHANNELS = 16`, `MSP.h:260`) and the request command (`MSP_RC = 105`, `MSP.h:47`), but nothing in `MSPManager` polls it today. The parent spec's own §11 file list already anticipated this exact need for a different (unimplemented) trigger mode: *"if using AUX trigger, add new `MSP_RC` polling + accessor."*

**Two candidate sources were considered:**

- **Option A — poll `MSP_RC` from the FC (chosen).** The FC already decodes whatever RX protocol the pilot is using (SBUS, CRSF, ELRS, etc.) into channel values and exposes them over MSP — the exact same mechanism INAV Configurator's own receiver tab uses. FF already polls other FC state this way (`MSPManager::local_altitude_cm()`, `MSPManager::getAnalogValues()`, `MSPManager.cpp:121-144, 148-...`), caching briefly and re-requesting each cycle. No new hardware, no new wiring, no new protocol decoder in FF.
- **Option B — a dedicated RX/SBUS input wired directly to the ESP32.** Rejected: FF is a companion computer, not a flight controller; it has no existing SBUS/CRSF decode stack, would require new hardware wiring per install, and would read the pilot's *raw* transmitter output rather than whatever the FC itself considers the current channel value (which already reflects the FC's own failsafe substitution, receiver linkup state, etc.) — Option A's values are strictly more meaningful to reason about than a second, independent decode of the same radio link.

**Verified against source:** `MSP::request()`/`MSP::recv()` (`MSP.cpp:236, 96-131`) already copy only `min(actualPayloadSize, maxSize)` bytes into the caller's buffer (`MSP.cpp:122-130`), leaving the rest of a `static`-scoped struct untouched. This means `MSP_RC` — whose real response length is `numChannels * 2` bytes and varies by receiver, unlike the fixed-size `msp_altitude_t`/`msp_analog_t` structs FF already polls — can be read into a full-size `static msp_rc_t` the same way, with any channel index beyond what the FC actually reports simply retaining its last-read (or zero-initialized) value. A channel that's never actually populated therefore reads as `0`, which per §3.1/§3.2 clamps to the `1000µs` endpoint and resolves to `-gap` — the same result as if the pilot had driven that channel fully to one extreme. Assigning a channel number the receiver doesn't actually populate is a pilot misconfiguration this spec doesn't detect or distinguish from real input (§3.2) — whether a channel counts as "assigned" at all is the only gate this spec applies; whatever value comes back once it's assigned is treated as real and mapped, never specially interpreted.

**A transient poll failure is not the same as a stale-but-real reading.** The FF↔FC MSP link is explicitly best-effort — the parent spec already designs around dropped frames (parent spec §2.1's framing, and the OSD/GVAR spec's own one-way/best-effort GVAR writes). A single `MSP_RC` request can simply time out on a busy link without the FC having reported anything at all. Since a channel's value is never itself treated as invalid (§3.2 — only assignment gates whether an axis is RC-controlled), a poll miss still needs a defined fallback of its own: rather than reading as `0µs` (which would clamp to the `1000` endpoint and swing the resolved offset to `-gap` for the duration of the dropped frame), the new accessor caches the last **successfully parsed** `msp_rc_t` (same pattern `local_altitude_cm()` already uses) and returns that cached snapshot on a poll miss. This keeps ordinary MSP congestion from producing a spurious swing to one extreme every time a frame drops — the resolved offset simply holds its last live value until the next successful poll (§9's `MSPManager` accessor carries this out).

### 2.2 Why the configured offset becomes a *bound*, not a fixed point
The parent spec's canonical offset (§7.3) is a single signed value per axis, expanded from a friendly AHEAD/BEHIND/LEFT/RIGHT/ABOVE/BELOW grid + a gap magnitude. This spec repurposes the **magnitude** of that same stored value as the RC-scaled range's bound (`gap = |config.ofsAxisM|`), and lets RC decide the **sign and position within that range** live, on whichever axes have a channel assigned. A pilot who configures "Behind, 15m" and assigns a channel to the longitudinal axis is really configuring "the slot moves live between 15m ahead and 15m behind" — the AHEAD/BEHIND label they picked in the grid UI stops describing a fixed point once RC control is active for that axis (see §8's UI-copy note). This was a deliberate design choice (matching the mapping the feature was requested with) over an incremental "RC as a trim added on top of the base offset" alternative — the direct mapping is stateless (no drift, no rate to tune) and gives the pilot a predictable, gimbal-slider-like feel: stick centered = centered slot, stick full one way = the full configured gap in that direction.

---

## 3. RC-to-Offset Mapping

### 3.1 Per-axis linear map
For each axis with a channel assigned (`channel >= 1`), given the FC-reported value `us` for that channel this cycle:

```
gap = |config.ofs{Long,Lat,Vert}M|      // the pilot's configured value, now a bound
us' = clamp(us, 1000, 2000)             // guard over/under-travel past the nominal endpoints
frac = (us' - 1500) / 500               // -1.0 .. +1.0
resolved = frac * gap
```

At `us = 2000`: `resolved = +gap` (e.g. the configured "15m" value, sign per §7.1's axis convention). At `us = 1500`: `resolved = 0`. At `us = 1000`: `resolved = -gap`. This matches the mapping described in the request exactly, generalized to all three axes (parent spec §7.1's longitudinal/lateral/vertical sign conventions apply unchanged — RC doesn't introduce a new frame, it just drives the existing one).

### 3.2 No-channel fallback
- **No channel assigned** (`channel == -1`, the default): the axis is untouched by this feature — `resolved = config.ofsAxisM`, exactly today's behavior. This is the *only* gate this spec applies to whether an axis is RC-controlled.
- **Channel assigned:** whatever value the FC reports for that channel this cycle — fresh or from the poll-miss cache (§2.1) — is treated as real input and run through §3.1's mapping unconditionally, including a raw value below `1000µs`. There is no separate "invalid reading" case: §3.1's `clamp(us, 1000, 2000)` already guards over/under-travel, so a reading below `1000` simply clamps to the `1000` endpoint and resolves to `-gap`, the same as a stick held fully at one extreme. A prior draft of this spec fell back to the plain configured static value below `1000µs`; that fallback is deliberately removed — with the FC-reported value itself now unable to distinguish "no signal yet" from "pilot commanded the extreme," assignment is the only signal this spec has to work with, and it uses that signal alone.
- This also means the "channel index beyond what the receiver actually sends" case from §2.1 does **not** fall back to the static value — an unpopulated channel index reads `0µs` from the cached `msp_rc_t`, which clamps to `1000µs` and resolves to `-gap`. Assigning a channel number the receiver doesn't actually populate is therefore a pilot misconfiguration this spec doesn't detect: the axis will simply sit pinned at `-gap` rather than silently reading as the static default.

### 3.3 Update cadence
`MSP_RC` is polled and cached the same way `local_altitude_cm()` already is (~100ms cache, `MSPManager.cpp:148-...`), so a read is fresh enough for every `FollowManager::loop()` cycle at the default 4Hz emit rate. **Polling is gated on at least one axis having a channel assigned** (`rcLongChannel/rcLatChannel/rcVertChannel != -1`) — a pilot who doesn't use this feature adds zero MSP traffic, mirroring the `-1`-disables-by-default pattern already established for the two GVAR fields (OSD/GVAR spec §3.4).

---

## 4. Live Geometry Safety Net ("Incorrect Setting")

### 4.1 Two related problems, not one
Unlike the base configured offset — which `applyConfig()` already guarantees satisfies the minimum-separation rules before it can ever be saved (parent spec §7.4) — an RC-scaled offset can reach **any** point inside its per-axis range, including all three axes centered (`0, 0, 0`), the degenerate collision slot §7.4 exists specifically to forbid. A pilot with all three axes RC-assigned centers all three sticks/channels far more easily than they'd ever accidentally type `0/0/0` into the web UI. Rejecting an individually-invalid candidate (§4.2) handles this half.

But rejecting only the *interior* of the forbidden zone isn't enough on its own, because the forbidden zone can have two disjoint valid sides — e.g. stacked directly over/under the leader (`long = lat = 0`), where `|vert| ≥ minVSepM` is valid on both the positive and negative side, with an invalid band in between. A pilot slowly driving the vertical channel from "fully below" to "fully above" spends the whole invalid band frozen at the boundary (§4.2), then the instant the reading crosses into the far side's valid region, the resolved offset **jumps straight there** — a large, single-cycle target discontinuity that, because the horizontal offset is `0` the whole time, passes directly through the leader's own position. The same discontinuity exists for the two purely-horizontal in-line cases too (`lat = vert = 0` sweeping `long` past the leader trail-to-lead; `long = vert = 0` sweeping `lat` side-to-side), governed by the plain `minSepM` sphere instead of the stacked `minVSepM` cone. §4.3 closes this second gap.

### 4.2 Layer 1: reject an individually-invalid candidate
Every cycle, after computing the RC-scaled triple (§3), it's validated with the same `offsetGeometrySane()` check `applyConfig()` already uses (`FollowManager.cpp:257-277` — minimum 3D separation, minimum stacked-vertical separation). A candidate that fails this check outright is never adopted — see §4.4 for what happens instead.

A per-axis nearest-valid clamp (moving just the offending axis to the closest boundary value, rather than discarding the whole triple) was considered and rejected: with two independent inequality constraints (a 3D-magnitude sphere and a stacked-vertical cone), there's no single well-defined "nearest valid point."

### 4.3 Layer 2: sign-lock — don't let a valid candidate jump across the forbidden zone
Even when a candidate triple independently passes Layer 1, it can still represent an unsafe *transition* from wherever the slot currently is, per §4.1. An axis is **sign-locked** — its candidate value is rejected even though the candidate triple as a whole is otherwise valid — when both of the following hold:

1. The candidate's sign for that axis differs from the last-known-good triple's sign for that axis (a genuine crossing, not just a magnitude change on the same side).
2. The **other two axes don't yet independently guarantee separation**: specifically, `coMag < minSepM`, where `coMag` is the combined magnitude of the other two axes, evaluated conservatively as the *smaller* of that pair's magnitude at the last-known-good triple and at the candidate triple (see below for why).

**Why `minSepM`, uniformly, for all three axes:** once the other two axes' combined magnitude is `≥ minSepM` on its own, `mag3d = sqrt(otherAxes² + thisAxis²) ≥ otherAxes ≥ minSepM` **regardless of what this axis does, including exactly `0`.** That's a mathematical guarantee, not a margin-of-safety guess, and it's true independent of which axis is being crossed — the vertical/stacked case doesn't need `minVSepM` here, because once horizontal clears `minSepM` it's already well past the much smaller `FOLLOW_STACKED_HORIZONTAL_EPSILON_M` (0.5 m) that makes the stacked rule apply at all, so `minVSepM` is moot at that point. This also means the release condition is expressed entirely in terms of a constant the pilot already configures (`minSepM`) rather than a new tunable this feature would have to introduce and explain.

**Why the conservative (smaller-of-two) `coMag`, not just the candidate's:** a single-axis lock evaluated only against the candidate's other-axis values doesn't fully cover two RC-assigned axes swinging *simultaneously* in the same cycle — e.g. longitudinal and vertical crossing past each other in the same cycle while lateral sits at a small fixed offset that alone doesn't clear `minSepM`. Requiring the *smaller* of the last-known-good and candidate co-magnitudes to already clear `minSepM` means both the state being left and the state being entered have to be independently safe before a crossing is allowed, which catches that case too. This is a cheap approximation, not full path/segment-vs-region collision math (rejected as an option in the prior discussion of this spec for the same complexity reasons as the auto-detour idea in §2.2) — see §10 for the residual gap this still doesn't close.

If any axis's crossing is locked, the **whole candidate triple** is treated as invalid for this cycle (same outcome as a Layer 1 rejection, §4.4) — not a partial mix of "accept the two unlocked axes, hold the third." This keeps there being exactly one freeze mechanism with two different reasons to trigger it, instead of two separate code paths to reason about.

### 4.4 Combined behavior: freeze the whole triple
- **Candidate passes both layers:** adopt it, and remember it as the new "last known good" triple.
- **Candidate fails either layer:** discard it for this cycle and use the **last known good triple** instead — frozen, all three axes together. The slot simply stops responding to further RC movement in the unsafe direction, holding the last position it was actually flying, rather than snapping somewhere the pilot didn't command.
- **Recovery:** as soon as a cycle's candidate triple passes both layers again (e.g. the pilot moves a stick off-center, or widens one of the other two axes past `minSepM` first), live tracking resumes immediately — there is no separate "unfreeze" action. Concretely, in the fully-stacked example from §4.1, crossing from below to above the leader requires the pilot to first move the longitudinal or lateral channel (if assigned) out past `minSepM`, cross the vertical channel while horizontal is safely clear, then bring horizontal back to center — the same way a real formation pass would have to route around the leader rather than through it. If neither of the other two axes has an RC channel assigned at all, that crossing isn't achievable live via RC — landing or using the web UI is the only way to flip a purely-vertical (or purely-horizontal) slot to its opposite side. This is treated as correct behavior, not a limitation: there is no safe live way to fly straight through the leader, so RC simply can't command that.

**Bootstrap value:** the very first "last known good" triple, before any RC-scaled value has ever been validated, is the configured static offset itself (`config.ofsLongM/ofsLatM/ofsVertM`) — guaranteed valid already, since `applyConfig()` never accepts a config that fails `offsetGeometrySane()`. `applyConfig()` also **resets** the remembered triple to the new config's static offset on every successful apply, since a config change (new gaps, a reassigned channel) may make the previously-frozen triple meaningless.

**Cycling the follow gate (switch off then on) deliberately does *not* reset the remembered triple.** Only `applyConfig()` does. A frozen/sign-locked slot stays exactly where it was across a gate cycle — there's no correctness reason it must reset, since the frozen triple remains geometrically valid regardless of gate state, and `resolveOffset()` simply isn't called while the gate is inactive (§11 notes the consequence for `LOCKED_HOLDING` too). This is a deliberate choice, not an oversight: a pilot cannot use the switch as a "clear the freeze" escape hatch in this iteration — only a genuine config change, or moving RC channels back into a jointly-valid combination, releases it (§4.4's Recovery bullet above).

### 4.5 Reporting: `conditionFlagsGvarIndex` code `2`
While the freeze in §4.4 is active — for either reason — the condition GVAR (OSD/GVAR spec §3.2) reports a new code, **`2` = "Incorrect Setting"**, instead of `0`. This is a live-flight-relevant distinction the existing binary "clamped/not clamped" altitude condition doesn't cover: it tells the pilot *their current stick/channel positions would produce an unsafe slot* — actionable information ("center your sticks back off," or "widen the other axis before crossing"), unlike a silent freeze the pilot has no OSD indication of. See §5 for how this interacts with the existing code `1` (altitude floor).

### 4.6 Pre-Arm RC Sanity Check
§10's "bootstrap trap" means a pilot whose RC channels don't agree with the static config's sign at the moment they first engage follow can get sign-locked away from their actual stick position, with no indication beyond the condition GVAR mid-flight. Rather than changing the sign-lock mechanism itself (deferred, §10), this adds a ground-side check that catches the common case before it ever matters: while the craft is disarmed, continuously evaluate whether the pilot's *current* RC input would trip the freeze if adopted right now, and warn if so.

The check enforces two independent rules, either of which failing raises the warning:

1. **Freeze-agreement (as originally specced):** the candidate would trip Layer 1 (§4.2) or Layer 2 (§4.3) if adopted right now.
2. **Exact-match (added after initial rollout):** on every axis with an RC channel assigned, the candidate must reproduce the configured static default (`config.ofsLongM`/`ofsLatM`/`ofsVertM`) within a small tolerance. This is stricter than rule 1 — a candidate can pass Layer 1/2 (e.g. because the other two axes already clear `minSepM` on their own, see §4.3) while still disagreeing with the static default, which rule 1 alone would miss. Rule 2 closes that gap: the pilot must only start using RC-driven offsets once airborne, never accidentally at the moment of arming.

**Mechanism:**
- Gated on `MSPManager::getState() == 0` (disarmed) — that accessor already exists (`MSPManager.cpp:29-38`, reads `MSP_MODE` bit 0) and needs no changes. Runs **independent of the follow gate/`GCS NAV`** — the point is to catch this before the pilot ever reaches for the switch, not just while follow is actively engaged.
- Every cycle while disarmed, compute the candidate offset triple from live RC readings (§3's mapping) for whichever axes have a channel assigned. Skip entirely if no axis has one — nothing for RC to disagree with, matching the `-1`-disables-by-default pattern used throughout this spec.
- Validate the candidate against Layer 1 (§4.2) and Layer 2 (§4.3), using the *current* `lastKnownGood` triple exactly as `resolveOffset()` would — but **read-only**. This must never write to `lastKnownGood`: it's a simulation of "what would happen if follow engaged right now," not a real state transition. Letting it mutate the frozen state would let a pilot's bench fidgeting silently prime (or corrupt) whatever `resolveOffset()` uses the moment they actually arm and engage.
- Separately, run the exact-match rule (`rcCandidateMatchesStaticDefault()`, `FollowManager.cpp`): for each RC-assigned axis only, `|candidate - config.ofsAxisM| <= FOLLOW_PREARM_MATCH_EPSILON_M` (0.05 m — absorbs RC read quantization/jitter, `frac` steps in `1/500` increments, without weakening the check into a sign-only match). Axes with no channel assigned always match trivially, since §3.2 already makes `resolved == config.ofsAxisM` for them.
- If either rule would reject the candidate, report `conditionFlagsGvarIndex` code `2` ("Incorrect Setting") — the same code §4.5 already defines, since both are instances of "an RC-driven slot the pilot shouldn't arm with as-is," just caught before flight instead of during it.

**Practical consequence of the exact-match rule:** because §3.1's mapping puts the static default at one *extreme* of an axis's range (`resolved = frac * gap`, so `frac = ±1` reproduces `±gap = config.ofsAxisM`, while center stick reproduces `0`), a pilot must hold each RC-assigned axis at full deflection toward the default's sign to clear the pre-arm check — center-stick, the most ergonomically "neutral" position, does **not** match a nonzero default and will correctly keep the warning lit. This is intentional, not a mapping bug: it forces a deliberate, verifiable stick position before arming rather than merely a same-sign one.

**Reporting path:** reuses `loop()`'s existing gate-inactive branch (`FollowManager.cpp:312-320`), which already calls `updateStatusGvars()` unconditionally with a hardcoded `0` today. That hardcoded value becomes this check's result instead, computed only while disarmed (armed-and-gate-inactive keeps reporting `0`, as today). No new GVAR and no new send-cadence logic — `updateStatusGvars()`'s existing change-or-heartbeat "due" logic (OSD/GVAR spec §3.3) already covers it. Also exposed directly on `GET /followmanager/status` as `rcPreArmCheckFailed` (§7), so it's visible in the web panel without a condition GVAR configured, the same reasoning as `rcSlotFrozen`.

**Advisory only — this never blocks or discourages arming.** FF has no existing mechanism to raise INAV's own arming-disable flags over MSP, and building one would be a materially larger, separate feature (noted, not solved, in §10). The warning is the entire scope: a pilot can arm and fly with it showing, at which point §4.1's original in-flight behavior applies unchanged.

**What this closes vs. what it doesn't:** the freeze-agreement rule alone only catches the subset of RC/default disagreement that would also trip Layer 1/2 — as shown in §4.3, a large enough static offset on the *other* two axes lets a sign-flipped RC axis pass through Layer 2 unflagged, so a pilot could arm with RC fully disagreeing with the configured default and see no warning at all. The exact-match rule closes that specific gap: it fires on *any* RC/default disagreement on an RC-assigned axis, independent of what the other axes are doing. It still does not change the sign-lock mechanism itself, and it does not close the underlying §10 bootstrap trap: a pilot who satisfies the pre-arm check, arms, and *then* moves a stick before actually engaging the follow gate can still land on the far side of a sign-lock the instant they do — pre-arm and gate-engage are two different moments, and this check only inspects the former. A pilot who ignores the warning and arms anyway (or drifts a stick after arming but before engaging) can still experience exactly the behavior §10's bootstrap-trap item describes.

---

## 5. Altitude Floor Interaction

The parent spec's altitude floor (§7.6) clamps the final *summed* commanded altitude, independent of what produced a too-low value — the leader flying low, a `BELOW` slot, or now, an RC-scaled vertical axis pushed toward the negative end of its range. The floor clamp itself is unchanged by this spec: **the floor always wins**, regardless of cause — RC can never command the follower below `minAltM`, exactly like the leader's own altitude never can today.

What changes is *which condition code gets reported* when the floor triggers.

### 5.1 Distinguishing "RC caused it" from "the leader/baseline caused it"
A naive proxy — "report `Incorrect Setting` whenever the vertical axis has a channel assigned and the floor is clamping" — was considered and rejected: it would misattribute a floor clamp to RC even when the pilot's RC input is near-neutral and the *leader's own* altitude is what actually dropped, just because RC happens to be enabled for that axis at all.

Instead, each cycle where the floor clamp is active, `FollowManager` cheaply computes **what the commanded altitude would have been using the plain configured static vertical offset** (`config.ofsVertM`, ignoring any RC scaling) instead of the actual (possibly RC-scaled) value used for the real emitted waypoint:

- If the **static-offset altitude would also have clamped** — the leader/baseline situation alone is enough to trigger the floor, with or without RC's help — report code `1` ("Alt Floor"), exactly as today.
- If the **static-offset altitude would *not* have clamped** — RC's live vertical value is specifically what pushed the commanded altitude below the floor — report code `2` ("Incorrect Setting") instead.

This degrades correctly with no special-casing when the vertical axis has no channel assigned: the "actual" and "static" values are then identical by construction (§3.2), so the comparison always agrees and the floor clamp — when it happens — always reports as plain "Alt Floor," unchanged from today's behavior.

### 5.2 Priority when both the geometry freeze (§4) and the floor clamp are active
Both conditions map to the same code (`2`), so there's no conflict to arbitrate: if §4.4's freeze is active, the frozen triple is a previously-*geometry*-valid combination (it already passed both layers of §4 when it was captured), but nothing prevents a valid-geometry triple from still summing, together with the leader's live altitude, to something below the floor — in which case both mechanisms are simultaneously "true," and both would independently select code `2` anyway.

### 5.3 Full condition code table

| Code | Meaning | Trigger |
|---|---|---|
| `0` | None | Neither of the below |
| `1` | Alt Floor | Commanded altitude clamped to `minAltM`, and the same clamp would have happened using the plain configured (non-RC-scaled) vertical offset — i.e. not attributable to live RC input |
| `2` | Incorrect Setting | Either: the RC-scaled offset triple failed Layer 1 or was blocked by Layer 2's sign-lock this cycle and is being held at its last known-good combination (§4), **or** the commanded altitude is clamped to `minAltM` *and* would not have been using the plain configured vertical offset — i.e. attributable to the pilot's live RC vertical input (§5.1) |

---

## 6. Configuration Model

Three new `FollowRuntimeConfig` fields (`FollowManager.h:36-68`), following the existing `statusGvarIndex`/`conditionFlagsGvarIndex` pattern exactly:

| Field | Type | Default | Meaning |
|---|---|---|---|
| `rcLongChannel` | `int16_t` | `-1` (disabled) | 1-based RC channel number driving the longitudinal axis, or `-1` |
| `rcLatChannel` | `int16_t` | `-1` (disabled) | 1-based RC channel number driving the lateral axis, or `-1` |
| `rcVertChannel` | `int16_t` | `-1` (disabled) | 1-based RC channel number driving the vertical axis, or `-1` |

**Range:** `-1`, or `1`–`16` (`MSP_MAX_SUPPORTED_CHANNELS`, `MSP.h:260`). Validated in `applyConfig()` the same way `statusGvarIndex`/`conditionFlagsGvarIndex` are (`FollowManager.cpp:558-567`). No firmware-side check that the same channel is assigned to more than one axis — same precedent as the GVAR index collision guard (OSD/GVAR spec §3.4): a front-end-only warning (§8), not a wire-level invariant.

**1-based, not 0-based:** matches how a pilot already thinks about "Channel 5" in their transmitter/INAV Configurator's receiver tab. Internally, `channelValue[channel - 1]` indexes the 0-based `msp_rc_t` array (§2.1).

**Compile-time defaults** (`FollowConfig.h`, `#ifndef`-guarded like every other key in that file): `FOLLOW_RC_LONG_CHANNEL`, `FOLLOW_RC_LAT_CHANNEL`, `FOLLOW_RC_VERT_CHANNEL`, each `-1`.

**EEPROM:** add the same three fields as `int16_t` to `FollowEepromRecord` (`FollowManager.h:95-118`) — no width-reduction conversion needed, they're channel numbers, not the meter/degree fields that motivate `FollowEepromRecord`'s `double`→`int16_t` narrowing (`FollowManager.h:79-93`). Bump `FOLLOW_EEPROM_VERSION` from `3` to `4` (same mechanism the OSD/GVAR spec's addition already used to go from `2` to `3`).

**Config JSON (`configJson()`/`applyConfig()`, `FollowManager.cpp:494-586`):** add all three fields to both, following the existing pattern for every other field.

---

## 7. Status Endpoint Additions

`GET /followmanager/status` (`statusJson()`, `FollowManager.cpp:391-413`) gains four fields so the web panel (and a pilot bench-testing before flight) can see what the RC scaling is actually doing, not just the static config:

- `liveOffset: { longM, latM, vertM }` — the offset triple actually used for this cycle's waypoint (post-RC-scaling, post-freeze) — distinct from `ofsLongM/ofsLatM/ofsVertM` in `configJson()`, which remain the static, RC-independent configured value/bound. Only populated while follow is actively emitting (same `haveLastTarget`-gated pattern the existing `lastTarget` field already uses).
- `rcSlotFrozen: bool` — whether §4.4's freeze is currently active, for either reason (mirrors `conditionFlagsGvarValue == 2`, exposed directly so the web UI doesn't need a GVAR configured to show it). Same gating as `liveOffset`.
- `rcPreArmCheckFailed: bool` — §4.6's pre-arm advisory result: `true` if either the freeze-agreement rule (Layer 1/2) or the exact-match rule (candidate reproduces `config.ofsAxisM` on every RC-assigned axis) fails. Populated with the opposite gating from the two fields above: only meaningful (and only computed) while the craft is **disarmed**, since that's the only time §4.6 runs. `false` while armed, and `false` whenever no axis has an RC channel assigned.
- `preArmCandidateOffset: { longM, latM, vertM }` — the RC-scaled candidate triple §4.6's check evaluated this cycle (the same read-only `resolveCandidateOffset()` result feeding `rcPreArmCheckFailed`), so a pilot bench-testing with the craft disarmed can see actual numbers — e.g. confirm sticks centered maps to `0,0,0` and full deflection maps to the configured gap — not just a pass/fail bool. Same gating as `rcPreArmCheckFailed`: present only while disarmed with at least one RC axis assigned, absent (not just zeroed) otherwise so a stale reading from a prior disarmed cycle can never be mistaken for a live one.

---

## 8. Web UI (`html/follow.js`)

New "RC Axis Control" panel, modeled on the existing "OSD Status (GVAR)" panel (`follow.js:284-294`):

- Three `<select>` dropdowns — one per axis — `Disabled` (`-1`) plus `1`–`16`, following the same options-array pattern as `gvarIndexOptions` (`follow.js:81`) but with the RC-appropriate 1–16 range rather than the GVAR fields' `0`-`7` (INAV's GVAR count is a different, unrelated limit from `MSP_MAX_SUPPORTED_CHANNELS`).
- Client-side-only duplicate-channel warning, mirroring `validateConfig()`'s existing GVAR-collision check (`follow.js:58-60`): if the same non-`-1` channel is assigned to more than one axis, block Save with an inline error, the same way a GVAR collision is blocked today. No server-side equivalent, matching that precedent.
- A short static note that once an axis has a channel assigned, the configured gap for that axis becomes a live-adjustable **range**, not a fixed point — so the AHEAD/BEHIND-style friendly-grid label for that axis (§2.2) is read as "the maximum in that direction," not "where the craft currently is." This is UI copy, not a data-model change (mirrors the parent spec's heading-mode label-switching precedent, §10.3).
- A short static note on the sign-lock behavior (§4.3): crossing a stacked/in-line axis from one side of the leader to the other requires first widening one of the other two RC-assigned axes past Min Separation — the slot won't fly through the leader to get there. Framed as expected behavior, not a bug to troubleshoot.
- Status panel additions: show `liveOffset` alongside the existing `lastTarget` readout, and render an inline warning when `rcSlotFrozen` is true (independent of whether a condition GVAR is even configured) so a pilot bench-testing without OSD wiring can still see the freeze happening.
- A separate, clearly-labeled pre-arm warning banner driven by `rcPreArmCheckFailed` (§4.6, §7) — distinct from the `rcSlotFrozen` warning above, since it means something different (a ground-side "your sticks don't match" advisory, not a live in-flight freeze) even though both ultimately trace back to the same underlying check.
- Inline hint when a channel is assigned to an axis whose configured gap is `0`: that assignment currently has no effect (§11), so surface it next to the dropdown rather than leaving it silently inert.

**Non-goal, explicitly:** no smoothing/rate-limiting control is exposed — the mapping is direct every cycle (§3.1), matching how every other live-computed value in `FollowManager` (e.g. `resolveCourseDeg()`) is also unfiltered today. If RC jitter or a noisy channel proves disruptive in practice, a deadband or slew-rate limit is a candidate follow-up, deliberately deferred rather than speculatively designed here (see §10).

---

## 9. Files / Modules to Change

1. **`src/lib/MSP/MSP.h`** — no changes; `msp_rc_t`/`MSP_RC` already exist (§2.1).
2. **`src/lib/MSP/MSPManager.{h,cpp}`** — add a cached `MSP_RC` poll (mirrors `local_altitude_cm()`'s pattern, `MSPManager.cpp:148-...`) and an accessor, e.g. `bool getRcChannelUs(uint8_t channel1Based, uint16_t *outUs)`, returning `false` (and leaving `*outUs` untouched) only if not connected to a flight controller or `channel1Based` is out of `msp_rc_t`'s range. A transient poll miss on an otherwise-connected FC is **not** one of those failure cases — it returns the last successfully parsed value from the cache instead (§2.1), the same way `local_altitude_cm()`'s own cache already rides out a single dropped frame.
3. **`src/lib/Follow/FollowConfig.h`** — add `FOLLOW_RC_LONG_CHANNEL`/`FOLLOW_RC_LAT_CHANNEL`/`FOLLOW_RC_VERT_CHANNEL` defaults (§6).
4. **`src/lib/Follow/FollowManager.h`** — add the three fields to `FollowRuntimeConfig` and `FollowEepromRecord` (§6); add whatever private state §4's two-layer check needs (a remembered "last known good" `FollowOffset` for the freeze itself — §4's Layer 2 sign-lock test is computed fresh each cycle from that same remembered triple plus the new candidate, no separate state needed); change `updateStatusGvars()`'s signature from `bool floorClamped` to an `int32_t conditionCode` (0/1/2) so its one caller in `loop()` can pass the already-resolved code from §5/§6 instead of a single bool.
5. **`src/lib/Follow/FollowManager.cpp`** — `resolveOffset()` (`FollowManager.cpp:175-178`) gains the §3 per-axis mapping and §4's two-layer check (Layer 1: `offsetGeometrySane()` re-run on the candidate; Layer 2: per-axis sign-lock test against the last-known-good triple using the conservative co-magnitude, §4.3), factored so its Layer 1/Layer 2 validation can be shared, read-only, with §4.6's pre-arm check (e.g. a private helper taking a candidate + the current `lastKnownGood` and returning pass/fail, called by both `resolveOffset()` — which then updates `lastKnownGood` on a pass — and the new pre-arm check, which never does); `loop()` (`FollowManager.cpp:300-377`) gains §5.1's static-vs-actual altitude comparison to pick between codes `1`/`2`, and its gate-inactive early return (`FollowManager.cpp:312-320`) gains §4.6's `MSPManager::getState()`-gated pre-arm check in place of the hardcoded `0` it passes to `updateStatusGvars()` today; a new private helper `rcCandidateMatchesStaticDefault(candidate, config)` implements §4.6's exact-match rule and is OR'd into `rcPreArmCheckFailed` alongside the freeze-agreement result; a new `FOLLOW_PREARM_MATCH_EPSILON_M` (`0.05`) constant, defined alongside `FOLLOW_STACKED_HORIZONTAL_EPSILON_M`, sets that rule's tolerance; `applyConfig()` (`FollowManager.cpp:523-586`) gains range validation for the three new fields and resets the "last known good" triple (§4.4); `toEepromRecord()`/`fromEepromRecord()` (`FollowManager.cpp:594-653`) carry the three new fields through; `configJson()` (`FollowManager.cpp:494-521`) and `statusJson()` (`FollowManager.cpp:391-413`) gain the new fields (§6, §7).
6. **`html/follow.js`** — new panel, status readout additions, client-side validation additions (§8).
7. **Config/target `.ini` files** — add the three new `build_flags` defaults (parent spec §9's pattern), all `-1`.

---

## 10. Open Questions

- **Deadband/hysteresis on the geometry freeze (§4.4):** currently unfreezes the instant a cycle passes both layers again, which could chatter (and re-send the condition GVAR) if a pilot's stick sits right at a safety boundary. Deferred rather than speculatively designed — worth revisiting if bench/flight testing shows it's disruptive in practice.
- **Residual gap in Layer 2's simultaneous-multi-axis coverage (§4.3):** the conservative (smaller-of-last-known-good-and-candidate) co-magnitude test closes the common two-channels-moving-together case, but it's still a per-axis approximation, not a true continuous path/segment-vs-region check across all three axes at once. A pathological combination of all three RC channels moving simultaneously in just the wrong way could in principle still thread a valid-to-valid jump this test doesn't catch. Full rigor here means the same trajectory-reasoning complexity as the auto-detour idea already rejected in §2.2, for what would in practice be a rare, hard-to-hit combination — flagged for awareness, not solved here.
- **Whether 1–16 is the right channel ceiling for every supported RX protocol.** `MSP_MAX_SUPPORTED_CHANNELS = 16` is FF's own MSP struct's fixed size (`MSP.h:260`); this matches what `MSP_RC` can report today regardless of the underlying RX protocol's own channel count, so it should already be correct, but is called out here for confirmation during implementation/bench testing rather than assumed silently.
- **Whether the static-vs-actual altitude comparison in §5.1 is worth its small added per-cycle cost** (one extra `slotToLatLon`-independent altitude sum, no extra MSP traffic) versus the simpler "channel assigned ⇒ always code 2" proxy that was rejected in §5.1 for correctness reasons — flagged here only because it's a slightly more involved computation than the rest of `loop()` does per condition check, not because the correctness reasoning is in doubt.
- **The bootstrap trap:** the very first "last known good" triple is the static config default (§4.4), which anchors Layer 2's sign-lock (§4.3) from the very first RC-evaluated cycle. If the pilot's transmitter is already sitting on the opposite side from the static default's sign the instant follow engages (e.g. static default is BEHIND but the stick is already at full AHEAD), and the slot is otherwise stacked/in-line on that axis, Layer 2 locks the very first live value away from wherever the pilot's stick actually is — with arguably no real safety benefit, since the first commanded waypoint after engaging is always a jump from the craft's current physical position regardless of RC, not a transition between two previously-live-tracked targets the way §4.1's problem is framed. **The mechanism itself is explicitly deferred rather than fixed in this iteration** — noted here so a future revision addresses it deliberately (e.g. a one-time exception on the first live cycle post-engage/post-`applyConfig()`) rather than it being rediscovered as a field-reported bug. §4.6's pre-arm advisory check mitigates the practical impact (a pilot who heeds the ground warning and centers/repositions their stick before arming never hits this), but a pilot who arms anyway with the warning showing still experiences the behavior described here unchanged.
  - **Narrowed, not closed, by §4.6's exact-match rule:** the original freeze-agreement-only pre-arm check had a real coverage gap here — per §4.3, a large enough static offset on the other two axes lets a sign-flipped RC axis clear Layer 2 with no warning at all, so a pilot could satisfy the *original* pre-arm check while still fully disagreeing with the static default on one axis, then hit this exact trap the moment they engaged follow. The exact-match rule added to §4.6 closes that specific hole: it fires on any RC/default disagreement on an RC-assigned axis, independent of the other axes' magnitude. What it still doesn't close: the pre-arm check only runs while disarmed, and a pilot can arm, then move a stick before actually flipping the follow-gate switch — the moment the two are decoupled is exactly the moment this check can't see. A one-time exception on the first live cycle post-engage (the fix floated above) remains the only way to close the underlying mechanism itself, and is still deferred.

---

## 11. Known Limitations & Operational Notes

Lower-stakes items — expected/deliberate behavior worth documenting so none of them get mistaken for bugs, rather than open design decisions like §10.

- **FC-substituted failsafe values are indistinguishable from real pilot input.** If the pilot's transmitter link to the receiver drops, INAV's own RX failsafe may hold the last channel values or substitute configured failsafe positions — either way, whatever comes back over `MSP_RC` (even a failsafe preset below `1000µs`, which §3.1 simply clamps to the `1000` endpoint like any other out-of-range reading) is treated as real input. FF has no way to tell that apart from genuine live input (§2.1, §3.2), so RC axis control simply keeps responding to whatever the FC reports during an RX failsafe. The real backstop is unchanged from today: the follow-mode gate/`GCS NAV` dropping out on a real failsafe event, not anything this feature adds.
- **No continuity guarantee across a `LOCKED_HOLDING` gap.** `resolveOffset()` (and thus the RC read/mapping in §3) only runs while `loop()` has a resolved peer — it doesn't execute during `ACQUIRING` or `LOCKED_HOLDING` (parent spec §6.3). If the pilot moves an RC channel substantially while the peer is stale, resuming tracking on reacquire can produce a large but *same-side* jump in one cycle — Layers 1/2 (§4) only guard against crossing the forbidden zone, not against a big move that stays validly on one side the whole time. Consistent with this spec's existing "no smoothing" non-goal (§1.4, §8) and with how the underlying position math itself doesn't smooth across a peer reacquire either.
- **RC axis control is per-aircraft, not per-formation.** Each follower reads `MSP_RC` from its own FC (§2.1) — one pilot's transmitter only retrims the slot of whichever aircraft's receiver it's bound to. Coordinating a live slot change across multiple followers in the same formation (parent spec's whole premise) requires either binding one transmitter identically across every follower's receiver (same channel number, same physical control) or adjusting each aircraft independently. Not a defect, just a scope reminder given the parent spec is inherently multi-craft.
- **The `FOLLOW_EEPROM_VERSION` 3→4 bump (§6) discards a pilot's entire previously-saved follow config on firmware upgrade, not just the three new fields** — `loadFromEEPROM()`'s version-mismatch check (`FollowManager.cpp:659`) treats any version difference as "nothing saved yet" and falls back to compile-time defaults wholesale. This is pre-existing behavior, identical to how the 2→3 bump already worked when the OSD/GVAR spec's fields were added — not new to this spec, but worth calling out in release notes so pilots know to re-save their configured slot once after updating.
- **A configured gap of exactly `0` on an RC-assigned axis is a silent no-op.** §3.1's mapping is `resolved = frac * gap`, so a `0` gap always resolves to `0` regardless of the channel's reading. Not a bug, but easy to mistake for one if a pilot assigns a channel to an axis they left at `CENTER`/`LEVEL` with no gap — worth a UI hint (§8) rather than just leaving it silently inert.
- **Mismatched gaps between RC-assigned axes can leave part of another axis's travel permanently unreachable**, even though each axis's static config passed validation individually. Example: a vertical gap of 5 m with the default `minVSepM` of 13 m — no vertical value that axis's channel can ever produce satisfies the stacked rule, so if lateral is also RC-assigned and gets driven toward center, Layer 1 (§4.2) correctly keeps freezing rather than emitting anything unsafe, but the pilot experiences it as a stick region that simply never responds. `applyConfig()` only validates the static point that's actually saved, not the full RC-reachable box a channel assignment opens up, so this can't be caught the same way today. Documented here as a known gap rather than built as a validation feature in this iteration; a non-blocking UI/`applyConfig()` warning when an RC-assigned axis's gap is below `minSepM` would be the natural follow-up if this proves confusing in practice.
