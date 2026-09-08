# Follow Mode — Pilot's Guide

This guide is for a pilot who already has two aircraft running FormationFlight
(FF) and INAV, can see each other on the radar HUD, and wants to turn on
autonomous **Follow Mode** — where one aircraft (the "follower") automatically
flies a formation slot behind/beside/above another aircraft (the "leader").

Everything here is done from FF's web UI (the **Follow** panel, reachable by
joining the aircraft's WiFi AP — SSID `iNav Radar-<chip ID>`, password
`inavradar` — and browsing to **`http://192.168.4.1/`**) and INAV
Configurator. You don't need to read any spec or source code to follow this
guide.

**Scope:** this only covers the *follower* side. The leader aircraft needs
nothing beyond ordinary FF/INAV setup — it just needs to be broadcasting its
position, which it's already doing if you can see it on the radar HUD.

**Craft type:** the follower must be a multirotor or a fixed-wing. Some differences
in functionality exist for fixed-wing vs multirotor followers.

---

## Table of contents

1. [How it works, in one paragraph](#1-how-it-works-in-one-paragraph)
2. [One-time INAV setup on the follower](#2-one-time-inav-setup-on-the-follower)
3. [The Follow panel, field by field](#3-the-follow-panel-field-by-field)
4. [Apply vs. Save to EEPROM](#4-apply-vs-save-to-eeprom)
5. [Flying it: engaging Follow Mode](#5-flying-it-engaging-follow-mode)
6. [Showing Follow status on your OSD (GVARs)](#6-showing-follow-status-on-your-osd-gvars)
7. [Trimming the slot live with RC channels](#7-trimming-the-slot-live-with-rc-channels)
8. [Troubleshooting](#8-troubleshooting)
9. [Slot geometry diagram](#9-slot-geometry-diagram)
10. [Speed Autothrottle (Fixed-Wing, Optional)](#10-speed-autothrottle-fixed-wing-optional)
11. [REST API reference: `/followmanager/config` and `/followmanager/status`](#11-rest-api-reference-followmanagerconfig-and-followmanagerstatus)

---

## 1. How it works, in one paragraph

While Follow Mode is engaged, FF continuously computes a 3D position —
a configurable offset (distance behind/ahead, left/right, above/below) from
wherever the leader currently is and however it's currently pointed — and
streams that position to the follower's own flight controller over MSP, at
up to 4 times a second. On the flight-controller side this rides entirely on
two stock INAV modes, **`NAV POSHOLD`** and **`GCS NAV`**, which together make
INAV treat FF's streamed position as a moving "fly here and hold" target.
FF never touches motors or bypasses INAV's own failsafe/RTH — it only ever
proposes a target position; INAV's own flight-control loop decides how to get
there and remains the authority on safety.

---

## 2. One-time INAV setup on the follower

Do this once per follower aircraft, in INAV Configurator, before you ever try
Follow Mode in the air.

### 2.1 Assign switches in the Modes tab

Follow Mode is gated on two INAV flight modes being active **together**:

| INAV mode | Purpose |
|---|---|
| `NAV POSHOLD` | Tells INAV to hold/fly to a position rather than obey raw stick input |
| `GCS NAV` | Tells INAV to take that position from an external device over MSP (that's FF) — this is INAV's stock "follow-me" hook |
| `HEADING HOLD` | If you plan to use [heading control](#heading-panel) modes other than "Off," also assign **`HEADING HOLD`** (sometimes labeled `MAG` in older INAV) to a switch — see the Heading section below for why. This is only really needed when the follower aircraft is a rotorcraft which can travel in a direction that is different from the direction of travel |

In Configurator's **Modes** tab, assign both `NAV POSHOLD`, `GCS NAV`, and `HEADING HOLD` to
an AUX switch. The simplest setup is to put all three on the *same* switch/range so
one flip engages the three  together — that single flip is what turns Follow Mode
on and off in flight (§5).


### 2.2 Confirm prerequisites

- The follower flies `NAV POSHOLD` cleanly on its own (calibrated compass,
  reliable GPS 3D fix) — verify this normally, with Follow Mode not involved,
  before ever combining it with Follow Mode.
- If your craft does not have a calibrated compass (along with the GPS), fly back and forth in a straight line to help the craft determine its orientation. You can confirm this by making sure the home arrow is point in the correct direction in the OSD.
- FF is connected to the flight controller over its configured MSP UART (the
  same link already used for the radar HUD).
- Both aircraft can already see each other on the radar HUD — i.e. peer
  telemetry already works end-to-end.

---

## 3. The Follow panel, field by field

Open the **Follow** panel in FF's web UI at **`http://192.168.4.1/`** (see
§1 for how to join the aircraft's WiFi AP). It's organized into the sections
below.

### Status

Read-only. Shows whether the follow gate is currently active, the current
lock state (`IDLE` / `ACQUIRING` / `LOCKED` / `LOCKED_HOLDING`), which peer is
currently locked, and the last target position actually sent to the flight
controller. This is the fastest way to check things are working from your
phone/laptop before or after a flight — see [§8](#8-troubleshooting).

### Follow Slot

This is where you set *where* the follower flies relative to the leader. It
has two views — a toggle in the top-right switches between them, and they
both edit the exact same underlying values:

- **Friendly grid (default view):** three axis pickers —
  **Longitudinal** (Ahead / Center / Behind), **Lateral** (Left / Center /
  Right), **Vertical** (Above / Level / Below) — each paired with a **Gap**
  distance in meters. E.g. "Behind" + "15 m" means 15 meters behind the
  leader.
- **Advanced (raw offsets):** the same slot as three signed meter values —
  **Longitudinal Offset** (+ahead / −behind), **Lateral Offset** (+right /
  −left), **Vertical Offset** (+above / −below). Useful if you want a value
  that doesn't fit the Ahead/Behind/Center labels' mental model, or you're
  scripting config changes.

See [§9's diagram](#9-slot-geometry-diagram) below for a picture of what each
axis means, and how the leader's own heading rotates the whole slot with it.

The factory default is **Behind 15 m, Center, Above 10 m** ("chase-high") —
chosen because it keeps the follower clear of the leader's rotor wash while
still being a sane geometry to bench-test with the leader sitting still or 
moving on the ground.

### Safety Bounds

| Field | Meaning |
|---|---|
| **Min Separation** | Smallest allowed straight-line (3D) distance from the leader. A slot that works out to less than this is rejected — this exists specifically to forbid an accidental "sit exactly on top of the leader" (0,0,0) configuration. |
| **Min Vertical Separation (when stacked)** | Specifically when the slot is directly above/below the leader with no horizontal offset at all, the minimum vertical gap required. This is set well above the pure physical clearance you'd expect (13 m by default) because both aircrafts' GPS altitude has real error, and a stacked slot is the one geometry where that error alone could cause a collision. |
| **Max Target Distance** | If the leader is ever reported farther away than this, Follow Mode stops emitting rather than letting the follower chase indefinitely across an implausible distance (e.g. a bad GPS reading). |
| **Min Altitude Floor** | The lowest altitude (home-relative) the follower will ever be commanded to, regardless of what the leader is doing. If the leader descends, lands, or you've configured a "Below" slot, the commanded altitude is clamped up to this floor rather than letting the follower fly toward or below ground level. It's a clamp, not a full stop — the follower keeps tracking the leader horizontally and just holds at the floor altitude. |
| **Min Course Speed** | Below this ground speed, the leader's reported heading is too noisy to trust for orienting the slot (a stationary or near-stationary GPS course jitters unpredictably). Below this speed, FF freezes the slot's orientation at the last heading it trusted, rather than following that jitter. |

The web UI blocks **Apply** if your Follow Slot values violate Min
Separation or Min Vertical Separation — you'll see an inline error rather
than being able to save an unsafe geometry.

### Heading

<a name="heading-panel"></a>

Controls which way the follower's *nose* points while following — independent
of which direction it's actually flying, since a multirotor can translate
sideways/backwards without yawing.

| Mode | Behavior |
|---|---|
| **Off** | Don't touch heading at all — leave it wherever pilot stick input / previous mode left it. (Probably best when the follower is a fixed-wing) |
| **Direction of Travel** | Nose points the way the leader is currently heading (not necessarily the way the follower itself is moving). |
| **Point at Leader** | Nose always points toward the leader's live position — useful for keeping a camera aimed at the leader regardless of formation position. |
| **Fixed Compass Heading** | Holds an absolute compass heading you enter (0° = North, 90° = East), regardless of the leader. |
| **Offset From Course** | Like Direction of Travel, but with a configurable degree offset added — e.g. +90° points the nose 90° clockwise from the leader's heading. |

**Important:** any mode other than "Off" requires INAV's **`HEADING HOLD`**
box to be active on the follower (assigned in Configurator's Modes tab,
§2.1) — this is a separate mode from `NAV POSHOLD`/`GCS NAV`, and heading
commands are silently ignored by the flight controller if it's not switched
on. If you configure a heading mode and the nose isn't doing what you expect,
this is the first thing to check.

### Trigger & Target

| Field | Meaning |
|---|---|
| **Trigger Mode** | Read-only, shown for reference. How Follow Mode gets switched on — the shipped default is `GCSNAV`, meaning the gate is simply "is `GCS NAV` currently active on the flight controller" (§2.1's switch). The other option has not yet been implemented. |
| **Target Peer** | Which aircraft to follow. "First Active" (the default) locks onto whichever peer is first seen once you engage Follow Mode. If you have more than two aircraft in the air, you can instead pin this to a specific peer's ID so there's no ambiguity about which one gets followed. |
| **Emit Rate** | How often (times per second) the *the follower* sends the target position to the craft — higher gives smoother tracking  of the leader craft but to high to swamp the iNav processor and lead to dropped messages. |
| **Peer Timeout** | How long without hearing from the locked leader before its telemetry is considered stale. When this trips, the follower holds its last commanded position rather than continuing to chase a stale reading (see the lock-state note below). |

**On peer locking:** once Follow Mode engages, the follower locks onto one
specific leader and **does not automatically switch to a different aircraft**
if that leader is lost — even if other peers are visible. If the locked
leader's telemetry goes stale, the follower holds position and waits for that
*same* aircraft's telemetry to come back; it never silently starts following
someone else. To deliberately re-target, either change **Target Peer** (this
forces a fresh lock) or cycle the follow switch off and on.

---

## 4. Apply vs. Save to EEPROM

The panel has two buttons at the bottom, and the difference between them
matters:

- **Apply** — sends your changes to the aircraft and puts them into effect
  **immediately**, live, without needing to reboot or reflash. They are
  **not** persisted: if the flight controller/FF board reboots or loses
  power, your changes are gone and it reverts to whatever was last actually
  saved (or the firmware's compiled-in defaults, if nothing was ever saved).
- **Save to EEPROM** — does the same live-apply as above, and then *also*
  writes the configuration to persistent storage on the FF board, so it
  survives a reboot/power cycle.

**Workflow:** use **Apply** while you're experimenting — try a slot, fly it
(or bench-test it), tweak, repeat — with no commitment. Once you're happy
with a configuration, hit **Save to EEPROM** once to lock it in as your new
default. If you only ever hit Apply, you'll need to re-enter your settings
after every reboot.

---

## 5. Flying it: engaging Follow Mode

1. Confirm both aircraft are powered on, connected to FF's WiFi if you need
   to check status, and visible to each other on the radar HUD.
2. On the follower, arm as normal.
3. Flip the switch you assigned in §2.1 to activate **`NAV POSHOLD`** +
   **`GCS NAV`** + **`HEADING HOLD`** (for rotorcraft) together.
4. That's the entire trigger — as soon as `GCS NAV` goes active, FF's Status
   panel should show the lock state move from `IDLE` → `ACQUIRING` →
   `LOCKED` (typically within a second or two once a peer is visible), and
   the follower should begin flying toward its configured slot relative to
   the leader.
5. To hand control back to yourself, flip the switch off. This immediately
   stops FF from sending target updates and drops the lock — INAV's normal
   stick control (or whatever mode you switch to) takes over exactly as it
   would for leaving any other nav mode.

**First flight recommendation:** don't discover your configured slot's
behavior for the first time in the air. Bench-test it first (there are
dedicated bench/HITL testing guides in `docs/explainers/` for anyone
comfortable with that level of setup), and on the first real flight, use a
generous, low-risk slot (e.g. the "chase-high" default, trailing well behind
and above) at low leader speed, with the follow switch within easy reach the
entire time.

**Screenshot placeholder:** *[FPV goggles — normal formation flight, follower
tracking the leader at the configured slot]*

### Testing Follow Mode with only one aircraft

Don't have a second aircraft to fly as the leader? FF can generate a **fake,
software-only leader** in its peer table instead of waiting for real
telemetry from a second aircraft — enough to bench-test your slot geometry,
heading mode, and RC trim on the ground, props off, before you ever need a
real formation partner.

This is done from a terminal with `curl` (or any HTTP client), not from the
Follow panel — join the aircraft's WiFi the same way you would to open the
panel (§1), then:

```bash
# Turn on a single fake leader flying a repeating hexagon patrol, centered on
# wherever your own aircraft's GPS currently reports itself to be
curl -d "index=1&sideLength=150&speed=8" http://192.168.4.1/peermanager/spoof
```

That starts a fake peer walking a 150 m-per-side hexagon at 8 m/s — you don't
need to know or compute any coordinates yourself. Open the Follow panel's
Status section (§3) and you should see the fake leader show up and get
locked onto, with `Last Target` visibly updating as it moves around its
patrol path — a real, moving target to check your configured slot tracks
correctly, without needing a second physical aircraft in the air.

The fake leader's altitude moves too, not just its lat/lon: it starts 10 m
above your own aircraft's current altitude, climbs gradually up to 100 m at
the halfway point of the patrol loop, then gradually descends back down to
that starting altitude by the time the loop closes — useful for checking
that your slot's vertical tracking (and Min Altitude Floor, §3) behaves
correctly against a target that's actually climbing and descending, not just
moving sideways.

(`curl -X POST http://192.168.4.1/peermanager/spoof` with no other
parameters instead generates five *stationary* practice peers in a ring
around you — a quicker way to check "a leader shows up and gets locked onto"
works, but not useful for watching the follower actually chase anything,
since nothing in that ring moves.)

**Safety:** this is a bench-test tool — treat it exactly like any other
bench test in §5 above (props off; only arm if you fully understand what
you're doing and why). The fake leader is not a real aircraft.

There's currently no "turn spoofing off" endpoint — reboot the aircraft to
return to real peer telemetry.

For the full technical rundown — every parameter, the geometry math behind
the patrol path, and a step-by-step checklist covering more advanced
scenarios like a stale/lost leader — see
[`docs/explainers/bench-testing-follow-mode.md`](explainers/bench-testing-follow-mode.md).

---

## 6. Showing Follow status on your OSD (GVARs) [Optional]

FF's web/status panel is not something you can see while flying goggles-in.
To get follow-state feedback directly on your OSD, FF can write small status
codes into INAV **Global Variables (GVARs)**, which you then turn into
on-screen text using INAV's own Programming Framework (Logic Conditions +
Custom OSD Elements). FF only ever writes a number — INAV does the rest.

**Requires INAV 9.0.0 or later** on the follower flight controller.
`MSP2_INAV_SET_GVAR` doesn't exist before that; on older/non-INAV firmware,
FF simply never sends anything — this whole feature stays inert, not broken.

### 6.1 Enable it in the Follow panel

In the **OSD Status (GVAR)** section, set:

- **Status GVAR Index** — which GVAR slot (`0`–`7`, or `Disabled`) carries
  the primary lock-state code.
- **Condition Flags GVAR Index** — which GVAR slot carries a secondary
  condition code (currently: whether the altitude floor is actively clamping
  the commanded altitude, or an RC-related condition — §7). Must be a
  *different* slot than Status GVAR Index; the UI blocks picking the same one
  twice.

Leave either on `Disabled` (the default) if you don't want that indicator —
zero MSP traffic is sent for a disabled slot.

| GVAR value (Status) | Meaning |
|---|---|
| `0` | Follow gate inactive — nothing to show |
| `1` | `ACQUIRING` — searching for a leader to lock onto |
| `2` | `LOCKED` — tracking normally |
| `3` | `LOCKED_HOLDING` — leader telemetry stale/lost, holding position |
| `4` | Peer identity mismatch caught — lock invalidated, needs a switch cycle |

| GVAR value (Condition Flags) | Meaning |
|---|---|
| `0` | No condition active |
| `1` | Altitude floor is actively clamping the commanded altitude |
| `2` | Target too far from this craft — Follow paused rather than chasing across an unbounded distance (see Max Target Distance, §4) |
| `3` | RC-driven slot is frozen at its last safe position, or the pre-arm RC check failed (§7) |

### 6.2 One-time INAV CLI setup

Pick your two GVAR indices first (whatever you entered in §6.1 above — the
snippets below use `<STATUS_GVAR_INDEX>` and `<CONDITION_FLAGS_GVAR_INDEX>`
as placeholders; substitute your actual numbers before pasting). Paste the
whole block into Configurator's **CLI** tab, then run `save`.

```
logic 0 1 -1 1 5 <STATUS_GVAR_INDEX> 0 1 0
logic 1 1 -1 1 5 <STATUS_GVAR_INDEX> 0 2 0
logic 2 1 -1 1 5 <STATUS_GVAR_INDEX> 0 3 0
logic 3 1 -1 1 5 <STATUS_GVAR_INDEX> 0 4 0
logic 4 1 -1 1 5 <CONDITION_FLAGS_GVAR_INDEX> 0 1 0
logic 5 1 -1 1 5 <CONDITION_FLAGS_GVAR_INDEX> 0 2 0
logic 6 1 -1 1 5 <CONDITION_FLAGS_GVAR_INDEX> 0 3 0
osd_custom_elements 0 1 0 0 0 0 0 2 0 "SEARCHING"
osd_custom_elements 1 1 0 0 0 0 0 2 1 "LOCKED"
osd_custom_elements 2 1 0 0 0 0 0 2 2 "HOLD LOST"
osd_custom_elements 3 1 0 0 0 0 0 2 3 "ID LOST"
osd_custom_elements 4 1 0 0 0 0 0 2 4 "ALT FLOOR"
osd_custom_elements 5 1 0 0 0 0 0 2 5 "TOO FAR"
osd_custom_elements 6 1 0 0 0 0 0 2 6 "BAD RC"
save
```


What this does:

- The seven `logic` lines each define a Logic Condition that evaluates "does
  the Status GVAR (first four) or Condition Flags GVAR (last three) currently
  equal this code."
- The seven `osd_custom_elements` lines each define a fixed piece of OSD
  text, visible only when its matching Logic Condition is true. Text is
  capped at 16 characters and INAV auto-uppercases it regardless of how you
  type it here — edit the quoted strings to whatever wording you prefer.
- Code `0` (gate inactive) and Condition Flags code `0` (no condition active)
  deliberately have no matching element — the OSD stays clean on flights
  where you never engage Follow Mode or no condition is active.

This example uses Logic Condition slots `0`–`6` and Custom OSD Element slots
`0`–`6`. If any of those are already used by something else on your aircraft
(another Logic Condition setup, existing custom elements), use free slots
instead and adjust the `osd_custom_elements` visibility values to match.

### 6.3 Place the elements on your OSD layout

`osd_custom_elements` only *defines* the elements — it doesn't position them.
In Configurator's **OSD** tab, the elements you just created appear in the
item list as `CUSTOM ELEMENT 1`–`7`; drag each onto the OSD preview wherever
you want it to appear on screen.

### 6.4 Verify

With the follower connected and Follow Mode engaged, watch the OSD (in
Configurator's OSD preview, or in goggles) as you cycle through follow
states — engage the switch, let it lock, then temporarily move out of radio
range of the leader (or stop the leader's telemetry) to see `HOLD LOST`
appear. Exactly one primary-indicator element should ever be visible at a
time; with the gate off, none should be.

**Screenshot placeholders:**

- *[FPV goggles — OSD showing "SEARCHING"]*
- *[FPV goggles — OSD showing "LOCKED"]*
- *[FPV goggles — OSD showing "HOLD LOST"]*
- *[FPV goggles — OSD showing "ID LOST"]*
- *[FPV goggles — OSD showing "ALT FLOOR" (or the RC-frozen condition) alongside a primary state]*

### 6.5 Raw coordinate debug GVARs

Separate from the status codes above, the Follow panel's **`debug`** toggle
writes the commanded target's position and heading into four fixed GVAR
slots every loop, for bench-testing in the goggles. It's a RAM-only toggle —
always back off after a reboot, never persisted.

| GVAR index | Value |
|---|---|
| `0` | North offset from the follower's own position, in **cm** (positive = target is north of you) |
| `1` | East offset from the follower's own position, in **cm** (positive = target is east of you) |
| `2` | Commanded altitude, home-relative, in **cm** |
| `3` | Commanded heading, absolute compass degrees (`0`–`359`) |

Indices `0`/`1` are a north/east offset from the follower's own position, not
absolute lat/lon — INAV's Custom OSD Elements can't display absolute lat/lon
anyway (raw GPS degrees × 1e7 is a ~9-10 digit number, but the widest
built-in numeric OSD part type clamps its display to 5 digits/`±99999`,
independent of the GVAR's own read/write range). A north/east offset in cm
is both small enough to fit that display and directly meaningful at a
glance — e.g. a "Behind 15 m" slot should read back as roughly `-1500` on
whichever of North/East corresponds to the leader's direction of travel.

**INAV clamps every GVAR write to that slot's configured range, and the
default range is `-32768..32767`** — fine for indices `2`/`3` and usually
fine for `0`/`1` too (a follow slot's gap is rarely more than a few hundred
meters), but if you're bench-testing with an unusually large offset and see
it pinned at exactly `32767`/`-32768`, widen that slot's range:

```
gvar 0 0 -2000000000 2000000000
gvar 1 0 -2000000000 2000000000
gvar 3 0 -2000000000 2000000000
save
```

(`gvar` takes four arguments — `index default min max`, not three — the
`default` here is `0`, INAV's own default for an unconfigured GVAR.)

---

## 7. Trimming the slot live with RC channels [Optional]

Landing, connecting to FF's web UI, editing a slot, and taking off again is
slow if you just want to nudge the formation gap mid-flight. The **RC Axis
Control** panel lets you assign an ordinary RC channel to each axis so you
can adjust the slot live from your transmitter while Follow Mode is engaged.
This is done by assigning an RC channel to a slider or a rotary knob so that
you can smoothly control the value of the channel.

### 7.1 What assigning a channel actually does

Once a channel is assigned to an axis, that axis's **configured gap stops
being a fixed point and becomes a live-adjustable range**, from the negative
of that gap to the positive of that gap:

- Slider/channel centered (1500µs) → the slot sits at the center of that axis
  (0 offset).
- Slider/channel at full deflection one way (2000µs) → the slot sits at the
  full **positive** configured gap for that axis.
- Slider/channel at full deflection the other way (1000µs) → the slot sits at
  the full **negative** configured gap.

Concretely: if you configure "Behind, 15 m" and assign a channel to the
Longitudinal axis, you're really configuring "this slot moves live between
15 m ahead and 15 m behind" — the "Behind" label you picked in the friendly
grid becomes the *maximum in that direction*, not a fixed position, the
moment a channel is assigned.

An axis left on **Disabled** (the default) is completely unaffected by this
— it stays exactly at its configured fixed value, same as today.

### 7.2 Setting it up

In the **RC Axis Control** panel, set **Longitudinal Channel** / **Lateral
Channel** / **Vertical Channel** to whichever RC channel number (as reported
by your receiver/flight controller) you want driving that axis, or leave
**Disabled**. Each axis needs a different channel — the UI blocks assigning
the same channel to more than one axis.

If you assign a channel to an axis whose configured gap is `0`, that channel
currently has no effect (there's no range to move within) — the panel shows
an inline warning if you do this.

### 7.3 Safety: the slot won't fly straight through the leader

RC input can, in principle, drive all three axes to `0, 0, 0` at once — the
same degenerate "on top of the leader" position the Safety Bounds section
forbids for the static configuration. FF guards against this live:

- Any candidate position that violates Min Separation / Min Vertical
  Separation is rejected outright for that cycle.
- Beyond that, if your slider input would cause the slot to **cross** from one
  side of the leader to the other (e.g. sweeping the vertical channel from
  "fully below" to "fully above" while horizontal is centered) while the
  *other* two axes don't yet provide enough separation on their own, that
  crossing is blocked too — it would otherwise mean the commanded position
  passes directly through the leader's position for an instant.

When either of these trips, the slot **freezes** at the last position it was
safely holding — it stops responding to further channel movement in the unsafe
direction — until you either move that slider back to a safe combination, or
widen one of the other RC-assigned axes past Min Separation first (the same
way a real formation pass has to route *around* another aircraft, not through
it). The Follow panel shows an inline warning (and, if you've set up the
Condition Flags GVAR, `3` on your OSD) whenever this freeze is active.

### 7.4 Pre-arm check

Because the very first "safe position" the freeze logic knows about is your
static configured slot, if your transmitter is already sitting somewhere
that disagrees with that configured slot the instant you engage Follow Mode,
you can get frozen away from your actual current slider position with no
warning beyond the OSD condition code.

To catch this before it matters, FF continuously checks — **while the
aircraft is disarmed only** — whether your current RC slider/channel
positions would produce a safe slot if Follow Mode were engaged right now,
and whether they reproduce your configured static default on every
RC-assigned axis. If either check fails, the Follow panel shows a red
pre-arm warning banner.

**This is advisory only — it never blocks arming.** But heed it: **the check
requires holding each RC-assigned axis at full deflection toward your
configured default's sign** — center-channel, which feels like the "neutral"
position, will *not* clear this check if your configured default for that
axis is nonzero. That's intentional: it forces you to deliberately confirm
channel position before arming, rather than assuming center is safe. Get your
channel into the position the warning wants before arming and engaging follow,
and this scenario never comes up.

---

## 8. Troubleshooting

| Symptom | Likely cause |
|---|---|
| Flipping the switch does nothing, Status panel stays `IDLE` | `NAV POSHOLD` + `GCS NAV` aren't both actually active — check INAV Configurator's Status tab, not just your transmitter switch position. |
| Status shows `ACQUIRING` and never moves to `LOCKED` | No peer visible yet, or the only visible peer has `id = 0`/is marked lost. Check the radar HUD / `Target Peer` peer list is populated. |
| Status shows `LOCKED_HOLDING` | The locked leader's telemetry went stale (radio range, or the leader stopped broadcasting). The follower is intentionally holding position, not searching for a new target — see the peer-locking note in §3. |
| Status shows lock dropping to `ID LOST`-style behavior | The same peer ID got reassigned to a different physical aircraft mid-flight (e.g. the original leader dropped out long enough to lose its radio slot). Cycle the follow switch to force a fresh lock. |
| Follower won't arm/engage at all with a "unsafe slot" warning | Your Follow Slot values violate Min Separation or Min Vertical Separation — widen the gap, or reduce Min Separation if that's genuinely appropriate for your aircraft size. |
| Follower holds well below/above where you expected | Check Min Altitude Floor — if the leader's altitude (or your vertical offset) would put the follower below that floor, altitude gets clamped up to it; this is intentional, not a bug. |
| Nose doesn't point where the Heading setting says it should | `HEADING HOLD` isn't active on the follower — see the note under [Heading](#heading-panel). This is a separate switch from `NAV POSHOLD`/`GCS NAV`. |
| RC channel doesn't move the slot at all | Either the axis is still `Disabled`, or its configured gap is `0` m (nothing to move within) — the panel flags the latter with an inline warning. |
| RC channel stops responding partway through its travel | You've hit the sign-lock freeze — see §7.3. Center the stick or widen another RC-assigned axis first. |
| Settings revert after a power cycle | You used **Apply** but never **Save to EEPROM** — see §4. |

If none of the above explains what you're seeing, the Status panel's
`Locked Peer` and `Last Target` fields are the most useful live diagnostic —
compare the reported target lat/lon/altitude against where you'd expect the
slot to be given the leader's current position and heading.

---

## 9. Slot geometry diagram

<a name="9-slot-geometry-diagram"></a>

The follow slot is defined relative to the **leader's current heading**, not
to compass directions — it rotates with the leader. "Behind" always means
behind wherever the leader is currently pointed, not south, or whatever
direction the leader happened to be facing when you configured the slot.

```
                                  leader's direction of travel
                                              ▲
                                              │
                         AHEAD (+ longitudinal)
                                              │
                    ┌─────────────────────────┼─────────────────────────┐
                    │                         │                         │
                    │        ▲ ABOVE (+ vertical, out of the page)      │
                    │                         │                         │
      LEFT ─────────┼─────────────────────────┼─────────────────────────┼───────── RIGHT
   (− lateral)      │                         │                         │       (+ lateral)
                    │                        LEADER                     │
                    │                       (origin)                    │
                    │        ▼ BELOW (− vertical, into the page)        │
                    │                         │                         │
                    └─────────────────────────┼─────────────────────────┘
                                              │
                         BEHIND (− longitudinal)
                                              │
                                              ▼

   Example: "chase-high" default = Behind 15m, Center, Above 10m
   → follower sits 15m behind and 10m above the leader, tracking
     whichever way the leader is currently facing.
```

|  | Friendly-grid label | Raw offset sign | Axis |
|---|---|---|---|
| Longitudinal | Ahead | positive (+) | along the leader's direction of travel |
| Longitudinal | Behind | negative (−) | " |
| Lateral | Right | positive (+) | perpendicular to travel, 90° clockwise |
| Lateral | Left | negative (−) | " |
| Vertical | Above | positive (+) | straight up |
| Vertical | Below | negative (−) | straight down |

For the full worked math behind this projection (how a track-relative offset
becomes an absolute lat/lon), see
[`docs/explainers/follow-target-geometry.md`](explainers/follow-target-geometry.md)
— that's implementation detail aimed at developers, not required reading to
fly this feature.

---

## 10. Speed Autothrottle (Fixed-Wing) [Optional]

<a name="10-speed-autothrottle-fixed-wing-optional"></a>

**This section only applies to a fixed-wing follower.** This feature assumes 
your fixed-wing follower is already flying `NAV POSHOLD` + `GCS NAV` acceptably 
well on its own before you add speed control on top of it; if it isn't, 
fix that first — everything below only ever adjusts throttle, never position.

### 10.1 What this adds

Follow Mode by itself only ever commands a **position** — it has no opinion
on how fast the follower gets there. On a fixed-wing follower that's often
too sluggish to hold a tight slot when the leader speeds up, slows down, or
turns: FF's position stream doesn't change, but the follower's own airspeed
does. Speed Autothrottle closes that gap by having FF compute a target ground
speed every cycle — matching the leader's own speed, then nudging it up or
down depending on whether the follower is lagging behind or running ahead of
its slot — and writing that number into an INAV Global Variable that drives a
Programming Framework throttle override on the follower's flight controller.

FF only ever writes two numbers: the target speed, and a 0/1 flag saying
whether autothrottle should be active right now. INAV does the rest — this
section is that one-time INAV setup, mirroring §6's OSD GVAR setup.

**Requires INAV 9.0.0+** (for the GVAR writes) and an **airplane mixer** on
the follower FC (checked automatically — see §10.3).

### 10.2 Enable it in the Follow panel

In the **Speed Autothrottle (Fixed-Wing)** section of the Follow panel, set:

- **Target Speed GVAR Index** — which GVAR slot (`0`–`7`, or `Disabled`)
  carries the commanded ground-speed setpoint (cm/s).
- **Autothrottle Engage GVAR Index** — which GVAR slot carries the 0/1 engage
  flag. Must be different from Target Speed GVAR Index, and from the §6
  Status/Condition Flags indices if you've also enabled those — the UI blocks
  picking the same slot twice.
- **Arm Channel** *(optional)* — an RC channel to use as a hardware autothrottle
  on/off switch, independent of the follow switch itself. Leave `Disabled` if
  you want autothrottle to run automatically whenever Follow Mode is locked
  onto a leader on a fixed-wing airframe, with no extra switch. If you assign
  a channel, autothrottle is only active while that channel's pulse width
  falls inside **Arm Range Min**/**Arm Range Max** (µs) — a range, not a
  single threshold, so it can describe a 2-way switch (wide range covering
  the whole high half of travel), a 3-way switch, or a specific position on a
  6-pos switch, whichever you've physically wired. Assigning an Arm Channel
  means you intend to use autothrottle, so the panel will also require you to
  explicitly fill in **Min Target Speed**/**Max Target Speed** below (see
  next bullet) — the firmware refuses to save an Arm Channel alongside their
  `0` defaults, so you can't accidentally arm the feature when you haven't 
  set a speed range. No default is provided because it can vary significantly
  from aircraft to aircraft.
- **Slot-Lag Correction Accel** — max closing acceleration/deceleration (in
  cm/s²) used to speed up/slow down beyond the leader's raw ground speed and
  correct for lagging or leading the follow slot. This follows a kinematic
  braking curve (`v = sqrt(2 · accel · distance)`), so it ramps up quickly
  when the follower is far from its slot and tapers off smoothly as it
  arrives, matching the leader's speed exactly once in slot — rather than a
  constant-rate correction that's equally aggressive at any distance.
  `0` (the default) means pure feedforward: just mirror the leader's speed
  exactly, no correction. Leave this at `0` until you've flown the feature
  once and have a specific lag/lead behavior you want to tighten up; higher
  values catch up faster but brake harder right before reaching the slot.
- **Min Target Speed** / **Max Target Speed** — hard floor/ceiling on the
  commanded speed, in m/s (the panel also shows the equivalent mph/km/h next
  to each field as a reference while you type). Both default to `0` and
  **must** be explicitly set once an Arm Channel is assigned — `0` is not a
  usable value, it's a deliberate placeholder forcing you to enter numbers
  for your specific airframe rather than inherit a default tuned for someone
  else's. **Min Target Speed is this feature's only stall-safety mechanism**
  — there is no dynamic sink-rate/rescue correction. Set it comfortably above
  this airframe's actual stall speed; roughly a third above stall is a
  reasonable starting point, not a validated number for your specific
  airframe. If Arm Channel is left `Disabled`, these two fields are ignored
  entirely and can stay at their `0` defaults.

If the follower FC isn't currently reporting an airplane mixer, the two GVAR
dropdowns and the tuning fields grey out with an explanatory tip — this is
advisory (you can still fill them in ahead of time), but the firmware itself
refuses to engage on anything other than an airplane mixer regardless of what
you've configured (§10.3's real gate).

### 10.3 The three-way engage gate

Autothrottle only ever engages when **all three** of these are true, checked
fresh every cycle with no latching:

1. Follow Mode has a leader actively **locked** (same lock state as §3's
   Status panel — `ACQUIRING`/`LOCKED_HOLDING`/gate-inactive all count as
   not engaged).
2. The follower FC is reporting an **airplane** mixer (checked automatically
   over MSP — not something you configure, it's read live from INAV).
3. The **Arm Channel** (§10.2), if you assigned one, is in its armed range.
   Leave it `Disabled` and this condition is always satisfied.

Losing any one of the three drops the engage flag to `0` and the aircraft
falls through to INAV's own regular `NAV POSHOLD` speed behavior — it never
holds a stale setpoint. This is what lets you run ordinary auto-follow
(position only) on a fixed-wing aircraft with autothrottle switched off
entirely, just by flipping the Arm Channel switch, with no web UI trip
required.

### 10.4 One-time INAV CLI setup

Pick your two GVAR indices first (§10.2 above — the snippets below use
`<E>` for Autothrottle Engage GVAR Index and `<T>` for Target Speed GVAR
Index; **substitute your actual numbers (0-7) before pasting, don't paste
`<E>`/`<T>` literally**). Paste the whole block into Configurator's **CLI**
tab, then run `save`.

```
logic 33 1 -1 1 5 <E> 0 1 0
logic 36 1 33 17 5 <T> 0 28 0

logic 39 1 33 14 6 3 0 3000 0
logic 40 1 33 17 4 39 0 2 0
logic 41 1 33 43 0 1800 4 40 0
logic 42 1 33 44 0 1250 4 41 0
logic 43 1 33 44 4 41 4 42 0
logic 44 1 33 29 4 43 0 0 0
logic 45 1 33 17 4 43 0 10 0
logic 46 1 33 15 4 45 0 100 0

pid 3 1 5 <T> 2 9 800 550 80 400

osd_custom_elements 0 2 171 2 172 18 46 2 33 ""
osd_custom_elements 1 1 0 18 36 2 144 2 33 "TARGET "

save
```

What this does:

- `logic 33` reads your Autothrottle Engage GVAR — this is the *only* Logic
  Condition INAV needs to know about FF's engage decision; everything
  upstream of it (lock state, airframe check, arm switch) already happened
  on the FF side before the GVAR was written.
- `logic 36` is just the "TARGET" OSD readout's unit conversion — cosmetic,
  not part of the control loop.
- `logic 39`-`46` are the throttle-output chain: take Programmable PID 3's
  output, offset and scale it into a servo-pulse range, and force-write it to
  the throttle channel. This clamps to `1250`-`1800`µs — that's an
  **actuator/hardware range**, not a speed bound; it has nothing to do with
  Min/Max Target Speed above, which are already fully resolved before the
  GVAR is written. This essentially limits the throttle to go between 25% and 80%
  to ensure that you don't drop the throttle too low that it could cause ESC
  de-sync issues when it throttles up quickly and you don't overdrive the motor
  and craft by being pegged at 100% throttle for a long time. You can change 
  those values if you feel comfortable pushing your craft further.
- `pid 3` is the actual speed-hold PID: setpoint reads your Target Speed
  GVAR directly, measurement reads the flight controller's own live ground
  speed. This is the loop that actually turns "FF wants X m/s" into a real
  throttle position.
- The two `osd_custom_elements` lines add a throttle-% readout and the
  "TARGET" speed readout to your OSD element list — drag them onto your OSD
  layout in Configurator's **OSD** tab the same way §6.3 describes for the
  status GVAR elements.

This example uses Logic Condition slots `33`/`36`/`39`-`46`, Programmable
PID slot `3`, and Custom OSD Element slots `0`/`1`. If any of those are
already used by something else on your aircraft, use free slots instead and
update the block's internal cross-references (the `33`s in the later lines
refer back to `logic 33`) to match.

#### Fallback, if your INAV build rejects the `pid 3` line above

**NOT FULLY TESTED. USE AT YOUR OWN RISK**

A small number of INAV builds may not accept a flight-telemetry (ground
speed) measurement source directly on a Programmable PID line. If `save`
fails or PID3's live measurement value in Configurator doesn't track your
GPS ground speed, use this fallback instead — only the `pid 3` line changes,
plus five extra Logic Conditions feeding a scratch GVAR (`GVAR1` below is
illustrative; pick any index that doesn't collide with `<E>`, `<T>`, or any
other FF-assigned GVAR):

```
logic 0 1 -1 1 2 31 0 1 0
logic 1 1 0 2 2 9 0 1000 0
logic 2 1 0 13 4 1 4 3 0
logic 3 1 -1 1 2 17 0 0 0
logic 4 1 2 14 2 9 0 0 0
logic 50 1 33 18 0 1 4 4 0

pid 3 1 5 <T> 5 1 800 550 80 400
```

This fallback uses one additional GVAR (`GVAR1` above) beyond the primary
block — worth remembering if you have other FF GVAR features (§6, `debug`)
enabled simultaneously and are running low on INAV's 8-GVAR budget.

### 10.5 Verify

With the follower on the ground (props off) or in the air on an airplane
mixer:

1. Confirm `Speed Autothrottle` in the Follow panel shows **Engaged: yes**
   once Follow Mode is locked onto a leader (real or [spoofed](#5-flying-it-engaging-follow-mode)),
   with a target speed roughly matching the leader's ground speed.
2. In Configurator's real-time monitor, confirm PID3's live setpoint tracks
   your Target Speed GVAR's value, and its measurement tracks the FC's own
   GPS ground speed.
3. Flip your Arm Channel switch (if assigned) off — confirm the Follow
   panel's **Engaged** indicator drops to **no** and PID3's setpoint stops
   updating within a second or two. Flip it back on and confirm it resumes
   immediately, with no re-lock needed.
4. If your follower FC isn't an airplane mixer, confirm **Engaged** stays
   **no** regardless of lock state — this is the airframe gate from §10.3
   working as intended, not a bug.

**Screenshot placeholder:** *[FPV goggles — OSD showing throttle % and TARGET
speed readouts while autothrottle is engaged]*

---

## 11. REST API reference: `/followmanager/config` and `/followmanager/status`

<a name="11-rest-api-reference-followmanagerconfig-and-followmanagerstatus"></a>

Everything in the Follow panel is just a UI over two JSON endpoints served by
the aircraft at `http://192.168.4.1/` (§1). This section is for anyone who
wants to script config changes or poll status directly (e.g. `curl`, a
ground-station script) instead of using the web UI — not required reading to
fly Follow Mode.

### `GET /followmanager/config`

Returns the currently-applied Follow configuration (whatever was last set via
**Apply**/**Save to EEPROM**, or the compiled-in defaults if nothing's been
applied yet). The same shape is accepted back on `POST /followmanager/config`
to change settings.

| Field | Type | Meaning |
|---|---|---|
| `ofsLongM` | number | Longitudinal offset, meters (+ahead / −behind) — §3's Follow Slot |
| `ofsLatM` | number | Lateral offset, meters (+right / −left) |
| `ofsVertM` | number | Vertical offset, meters (+above / −below) |
| `triggerMode` | string | `"GCSNAV"` or `"AUX"` — read-only, see §3 Trigger & Target |
| `targetPeer` | number | Pinned peer ID, or `0` for "First Active" |
| `emitHz` | number | Target-position send rate, times/sec |
| `peerTimeoutMs` | number | Leader-telemetry staleness timeout, ms |
| `minSepM` | number | Minimum allowed 3D separation from the leader, meters |
| `minVSepM` | number | Minimum vertical separation when the slot is stacked (no horizontal offset), meters |
| `maxTargetDistM` | number | Reject/pause the target if farther than this from the leader, meters |
| `minAltM` | number | Altitude floor (home-relative), meters |
| `minCourseSpeed` | number | Below this leader ground speed (m/s), freeze slot orientation at the last trusted heading |
| `headingMode` | string | One of `"OFF"`, `"COURSE"`, `"POINT_LEADER"`, `"FIXED"`, `"COURSE_RELATIVE"` — §3 Heading |
| `headingDeg` | number | Degrees; meaning depends on `headingMode` (absolute heading for `FIXED`, offset for `COURSE_RELATIVE`) |
| `statusGvarIndex` | number | GVAR index (`0`–`7`) for the primary status code, or `-1` if disabled |
| `conditionFlagsGvarIndex` | number | GVAR index (`0`–`7`) for the condition-flags code, or `-1` if disabled |
| `rcLongChannel` | number | 1-based RC channel driving the longitudinal axis, or `-1` if disabled — §7 |
| `rcLatChannel` | number | 1-based RC channel driving the lateral axis, or `-1` if disabled |
| `rcVertChannel` | number | 1-based RC channel driving the vertical axis, or `-1` if disabled |
| `targetSpeedGvarIndex` | number | GVAR index (`0`–`7`) for the autothrottle speed setpoint, or `-1` if disabled — §10 |
| `autothrottleEngageGvarIndex` | number | GVAR index (`0`–`7`) for the autothrottle engage flag, or `-1` if disabled |
| `autothrottleEnableRcChannel` | number | 1-based RC channel used as the autothrottle arm switch, or `-1` if unassigned (always armed) |
| `autothrottleEnableMinThresholdUs` | number | Lower bound (µs) of the arm switch's "armed" pulse-width range |
| `autothrottleEnableMaxThresholdUs` | number | Upper bound (µs) of the arm switch's "armed" pulse-width range |
| `speedCorrectionAccelCmS2` | number | Slot-lag correction: max closing acceleration/deceleration, cm/s² — `0` is pure feedforward (mirror the leader's speed) |
| `minTargetSpeedMps` | number | Lower clamp on the commanded autothrottle speed setpoint, m/s. Defaults to `0` (invalid on its own) and must be explicitly set > `0` once `autothrottleEnableRcChannel` is assigned |
| `maxTargetSpeedMps` | number | Upper clamp on the commanded autothrottle speed setpoint, m/s. Defaults to `0` and must be explicitly set > `minTargetSpeedMps` once `autothrottleEnableRcChannel` is assigned |
| `debug` | boolean | RAM-only debug-GVAR toggle; always reports `false` after a reboot, never persisted |

Example:

```json
{
  "ofsLongM": -15,
  "ofsLatM": 0,
  "ofsVertM": 10,
  "triggerMode": "GCSNAV",
  "targetPeer": 0,
  "emitHz": 4,
  "peerTimeoutMs": 1500,
  "minSepM": 8,
  "minVSepM": 13,
  "maxTargetDistM": 50,
  "minAltM": 3,
  "minCourseSpeed": 2,
  "headingMode": "POINT_LEADER",
  "headingDeg": 0,
  "statusGvarIndex": -1,
  "conditionFlagsGvarIndex": -1,
  "rcLongChannel": -1,
  "rcLatChannel": -1,
  "rcVertChannel": -1,
  "targetSpeedGvarIndex": -1,
  "autothrottleEngageGvarIndex": -1,
  "autothrottleEnableRcChannel": -1,
  "autothrottleEnableMinThresholdUs": 1700,
  "autothrottleEnableMaxThresholdUs": 2100,
  "speedCorrectionAccelCmS2": 0,
  "minTargetSpeedMps": 0,
  "maxTargetSpeedMps": 0,
  "debug": false
}
```

### `GET /followmanager/status`

Live, read-only snapshot — the same data backing §3's Status panel. Several
fields are only present when they're meaningful (e.g. `lastTarget` is omitted
until a target has actually been sent at least once).

| Field | Type | Meaning |
|---|---|---|
| `state` | string | `"IDLE"`, `"ACQUIRING"`, `"LOCKED"`, or `"LOCKED_HOLDING"` — §3 lock states |
| `gateActive` | boolean | Whether the follow gate (`GCS NAV`, §2.1) is currently active |
| `lockedId` | number | Currently-locked peer ID (`0` if none) |
| `lockedName` | string | Currently-locked peer's craft name |
| `lastTarget` | object *(present once a target has been sent)* | Last commanded waypoint — see below |
| `lastTarget.lat` | number | Latitude, degrees × 1e7 |
| `lastTarget.lon` | number | Longitude, degrees × 1e7 |
| `lastTarget.altCm` | number | Commanded altitude, home-relative centimeters |
| `lastTarget.headingDeg` | number | Commanded nose heading sent alongside the target, degrees |
| `lastTarget.ageMs` | number | Milliseconds since that target was sent |
| `statusGvarValue` | number *(present once sent, if `statusGvarIndex >= 0`)* | Last status code written to the OSD GVAR — §6.1's table |
| `conditionFlagsGvarValue` | number *(present once sent, if `conditionFlagsGvarIndex >= 0`)* | Last condition-flags code written to the OSD GVAR — §6.1's table |
| `platformType` | number | The follower FC's detected mixer platform type (`0`=Multirotor, `1`=Airplane, `2`=Helicopter, `3`=Tricopter, `4`=Rover, `5`=Boat) — §10.3's airframe gate reads this |
| `targetSpeedCmS` | number *(present once a target has been sent)* | Last computed autothrottle speed setpoint, cm/s — §10 |
| `autothrottleEngaged` | boolean *(present once a target has been sent)* | Whether autothrottle is currently engaged — §10.3's three-way gate |
| `liveOffset` | object *(present once a target has been sent)* | The actual offset in effect this cycle, after RC trim (§7) is applied |
| `liveOffset.longM` | number | Live longitudinal offset, meters |
| `liveOffset.latM` | number | Live lateral offset, meters |
| `liveOffset.vertM` | number | Live vertical offset, meters |
| `rcSlotFrozen` | boolean *(present once a target has been sent)* | Whether the RC-trimmed slot is currently frozen for safety — §7.3 |
| `preArmCandidateOffset` | object *(present only while disarmed and a candidate has been computed)* | What the slot would be right now if Follow Mode engaged — feeds the §7.4 pre-arm check |
| `preArmCandidateOffset.longM` / `.latM` / `.vertM` | number | Candidate offset components, meters |
| `rcPreArmCheckFailed` | boolean | Whether the §7.4 pre-arm advisory check currently fails |

Example (locked, mid-flight, RC axis control enabled):

```json
{
  "state": "LOCKED",
  "gateActive": true,
  "lockedId": 3,
  "lockedName": "Leader-1",
  "lastTarget": {
    "lat": 473567890,
    "lon": 85411234,
    "altCm": 4500,
    "headingDeg": 270,
    "ageMs": 120
  },
  "liveOffset": {
    "longM": -15,
    "latM": 2.5,
    "vertM": 10
  },
  "rcSlotFrozen": false,
  "rcPreArmCheckFailed": false
}
```
