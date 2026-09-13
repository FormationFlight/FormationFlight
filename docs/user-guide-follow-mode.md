# Follow Mode: pilot's guide

This guide is for a pilot who already has two aircraft running FormationFlight
(FF) and INAV, can see each other on the radar HUD, and wants to turn on
autonomous *Follow Mode*, where one aircraft (the "follower") automatically
flies a formation slot behind, beside or above another aircraft (the
"leader").

Everything here is done from FF's web UI (the *Follow* page) and INAV
Configurator. To reach the web UI, join the node's WiFi access point:
SSID `FormationFlight-<uid>`, open unless you set an AP password, then browse
to <http://192.168.4.1/>. If you have not set a node up before, start with
[`v2-getting-started.md`](v2-getting-started.md). You do not need to read any
spec or source code to follow this guide.

*Scope:* this only covers the *follower* side. The leader needs nothing beyond
ordinary FF and INAV setup. It has to be broadcasting its position with a valid
GPS fix, which it already is if you can see it on the radar HUD with a position
that moves.

*Group:* both aircraft must be set to the same group passphrase, under
*Settings > Node > Security*. Nodes with different passphrases cannot decode
each other's traffic at all, so the leader simply never appears.

*Craft type:* the follower must be a multirotor or a fixed wing. Some
functionality differs between the two.

> **Status.** This describes the v2 firmware, where Follow runs as
> `ff::FollowController` on the Node core. None of v2 has been validated on
> hardware yet: builds are compile-verified and the control law, geometry and
> config rules are covered by host tests, but nothing here has been flown.
> Bench-test before you fly it, and treat the safety margins as untested
> defaults rather than proven numbers.

---

## Table of contents

1. [How it works, in one paragraph](#1-how-it-works-in-one-paragraph)
2. [One-time INAV setup on the follower](#2-one-time-inav-setup-on-the-follower)
3. [The Follow page, field by field](#3-the-follow-page-field-by-field)
4. [Apply vs. Save](#4-apply-vs-save)
5. [Flying it: engaging Follow Mode](#5-flying-it-engaging-follow-mode)
6. [Showing Follow status on your OSD (GVARs)](#6-showing-follow-status-on-your-osd-gvars-optional)
7. [Trimming the slot live with RC channels](#7-trimming-the-slot-live-with-rc-channels-optional)
8. [Troubleshooting](#8-troubleshooting)
9. [Slot geometry diagram](#9-slot-geometry-diagram)
10. [Speed Autothrottle (fixed wing, optional)](#10-speed-autothrottle-fixed-wing-optional)
11. [Configuring Follow over the API](#11-configuring-follow-over-the-api)

---

## 1. How it works, in one paragraph

While Follow Mode is engaged, FF continuously computes a 3D position: a
configurable offset (distance behind or ahead, left or right, above or below)
from wherever the leader currently is and however it is currently pointed. It
streams that position to the follower's own flight controller over MSP, up to
4 times a second. On the flight-controller side this rides entirely on two
stock INAV modes, `NAV POSHOLD` and `GCS NAV`, which together make INAV treat
FF's streamed position as a moving "fly here and hold" target. FF never touches
motors or bypasses INAV's own failsafe and RTH. It only ever proposes a target
position; INAV's flight-control loop decides how to get there and remains the
authority on safety.

---

## 2. One-time INAV setup on the follower

Do this once per follower aircraft, in INAV Configurator, before you ever try
Follow Mode in the air.

### 2.1 Assign switches in the Modes tab

Follow Mode is gated on two INAV flight modes being active *together*:

| INAV mode | Purpose |
|---|---|
| `NAV POSHOLD` | Tells INAV to hold or fly to a position rather than obey raw stick input |
| `GCS NAV` | Tells INAV to take that position from an external device over MSP (that is FF). This is INAV's stock follow-me hook |
| `HEADING HOLD` | If you plan to use [heading control](#heading) modes other than "Off", also assign `HEADING HOLD` (sometimes labeled `MAG` in older INAV) to a switch. See the Heading section below for why. This is only really needed when the follower is a rotorcraft, which can travel in a direction different from the one it is pointed |

In Configurator's *Modes* tab, assign `NAV POSHOLD`, `GCS NAV` and
`HEADING HOLD` to an AUX switch. The simplest setup is to put all three on the
*same* switch and range so one flip engages all three together. That single
flip is what turns Follow Mode on and off in flight (§5).

### 2.2 Confirm prerequisites

- The follower flies `NAV POSHOLD` cleanly on its own (calibrated compass,
  reliable GPS 3D fix). Verify this normally, with Follow Mode not involved,
  before ever combining it with Follow Mode.
- If your craft does not have a calibrated compass alongside the GPS, fly back
  and forth in a straight line to help the craft determine its orientation. You
  can confirm this by checking that the home arrow points the right way in the
  OSD.
- FF is connected to the flight controller over its configured MSP UART, the
  same link already used for the radar HUD. The *Follow* page's *Live* card
  shows the FC variant and version once that link is up, and warns when it is
  not.
- Both aircraft can already see each other on the radar HUD, and both appear in
  the other's peer table on the *Dashboard*. That means the group passphrase
  matches and peer telemetry works end to end.

---

## 3. The Follow page, field by field

Open the *Follow* page from the sidebar of FF's web UI at
<http://192.168.4.1/>. It is organized into the cards below.

### Live

Read-only. Shows the trigger gate state, the current lock state
(`IDLE`, `ACQUIRING`, `LOCKED`, `LOCKED_HOLDING`), which peer is locked (name
and UID), the flight controller it found, the last target position actually
sent, the offset currently in effect, and the GVAR values last written. This is
the fastest way to check things are working from your phone or laptop before or
after a flight. See [§8](#8-troubleshooting).

It also shows a *Pre-arm* warning list, which is worth reading on the ground
rather than discovering in the air: no flight controller on MSP, no position
fix on this node, listen-only switched on (so the aircraft you are chasing
cannot see you), triggered but unlocked, or a failed RC pre-arm check (§7.4).

### Slot geometry

This is where you set *where* the follower flies relative to the leader. Each
of the three axes has a direction picker and a gap distance in meters:

- *Fore / aft*: Ahead, Center or Behind.
- *Left / right*: Left, Center or Right.
- *Up / down*: Above, Level or Below.

"Behind" plus "15 m" means 15 m behind the leader. The card also prints the
raw stored values underneath, as `ofsLongM` (+ahead / -behind), `ofsLatM`
(+right / -left) and `ofsVertM` (+above / -below) in meters, which are the same
numbers the API uses (§11).

See [§9's diagram](#9-slot-geometry-diagram) for a picture of what each axis
means and how the leader's own heading rotates the whole slot with it.

The factory default is *Behind 15 m, Center, Above 10 m* ("chase-high"), chosen
because it keeps the follower clear of the leader's rotor wash while still
being a sane geometry to bench-test with the leader sitting still or moving on
the ground.

### Trigger and target

| Field | Meaning |
|---|---|
| *Trigger* | Read-only, compiled in at build time (`FOLLOW_TRIGGER_MODE`). The shipped default is `GCSNAV`, meaning the gate is simply "is `GCS NAV` currently active on the flight controller" (§2.1's switch). The `AUX` alternative is not implemented and never asserts. |
| *Target peer* | Which aircraft to follow, as a peer UID (8 hex characters). Leave it empty and the follower locks onto the *nearest* followable peer at acquire time; the field lists the peers it can currently see, as buttons, so you rarely have to type one. Pin a UID if you have more than two aircraft up and want no ambiguity about which one gets followed. |
| *Update rate* | How often (Hz) the control cycle runs and a fresh waypoint goes to the flight controller. Faster tracks better but costs MSP bandwidth on the same link the rest of the telemetry uses. |
| *Peer timeout* | How stale the leader's last *position* may be before the lock goes to holding. An announce on its own does not count. |

*What makes a peer followable:* it has to be in the peer table, and its
beacons have to carry a valid GPS fix, and that position has to be newer than
*Peer timeout*. A leader that is powered up and beaconing without a fix is
never chased, no matter how healthy its link looks. That is deliberate: a
beacon without a fix carries no usable position.

*On peer locking:* once Follow Mode engages, the follower locks onto one
specific leader and *does not automatically switch to a different aircraft* if
that leader is lost, even if other peers are visible. If the locked leader's
telemetry goes stale, the follower holds its last commanded position and waits
for that same aircraft to come back. It never silently starts following someone
else. To deliberately re-target, either change *Target peer* (which forces a
fresh lock) or cycle the follow switch off and on.

Because a peer is identified by its 32-bit UID, and a UID belongs to one piece
of hardware, a lock can never be inherited by a different aircraft. The v1 slot
id could be reassigned mid-flight; that failure mode no longer exists, and
neither does the status code that reported it.

### Safety bounds

| Field | Meaning |
|---|---|
| *Min separation* | Smallest allowed straight-line (3D) distance from the leader. A slot that works out to less than this is rejected. This exists specifically to forbid an accidental "sit exactly on top of the leader" (0, 0, 0) configuration. |
| *Min vertical separation* | Specifically when the slot is directly above or below the leader with no horizontal offset at all, the minimum vertical gap required. It is set well above the physical clearance you would expect (13 m by default) because both aircraft's GPS altitude has real error, and a stacked slot is the one geometry where that error alone could cause a collision. |
| *Max target distance* | If the solved target is ever farther than this from the follower, Follow stops emitting rather than letting the follower chase across an implausible distance, for example after a bad GPS reading. |
| *Altitude floor* | The lowest altitude (home-relative) the follower will ever be commanded to, regardless of what the leader is doing. If the leader descends or lands, or you have configured a "Below" slot, the commanded altitude is clamped up to this floor rather than letting the follower fly toward or below ground level. It is a clamp, not a full stop: the follower keeps tracking the leader horizontally and holds at the floor altitude. |
| *Min course speed* | Below this leader ground speed, the leader's reported course is too noisy to trust for orienting the slot (a stationary GPS course jitters unpredictably). Below it, FF freezes the slot's orientation at the last course it trusted rather than following that jitter. |

The UI blocks *Apply* if your slot values violate *Min separation* or *Min
vertical separation*, showing an inline error rather than letting you save an
unsafe geometry. The firmware enforces the same rules independently, so the
same is true of a raw API call.

### Heading

Controls which way the follower's *nose* points while following, independent of
which direction it is actually flying, since a multirotor can translate
sideways or backwards without yawing.

| Mode | Behavior |
|---|---|
| *Off* | Do not touch heading at all. Leave it wherever pilot stick input or the previous mode left it. Probably best when the follower is a fixed wing. |
| *Direction of travel* (`COURSE`) | Nose points the way the leader is currently heading, not necessarily the way the follower itself is moving. |
| *Point at leader* (`POINT_LEADER`) | Nose always points toward the leader's live position. Useful for keeping a camera aimed at the leader regardless of formation position. |
| *Fixed compass heading* (`FIXED`) | Holds an absolute compass heading you enter (0 deg = North, 90 deg = East), regardless of the leader. |
| *Offset from course* (`COURSE_RELATIVE`) | Like Direction of travel, but with a configurable degree offset added. For example +90 deg points the nose 90 deg clockwise from the leader's course. |

*Important:* any mode other than Off requires INAV's `HEADING HOLD` box to be
active on the follower (assigned in Configurator's Modes tab, §2.1). This is a
separate mode from `NAV POSHOLD` and `GCS NAV`, and heading commands are
silently ignored by the flight controller if it is not switched on. If you
configure a heading mode and the nose is not doing what you expect, check this
first.

### RC axis control, GVARs and autothrottle

The remaining cards are optional features with their own sections:
[§7](#7-trimming-the-slot-live-with-rc-channels-optional) for RC axis control,
[§6](#6-showing-follow-status-on-your-osd-gvars-optional) for the status and
condition GVARs plus the debug GVAR toggle, and
[§10](#10-speed-autothrottle-fixed-wing-optional) for fixed-wing autothrottle.

---

## 4. Apply vs. Save

Every configuration card has the same two buttons, and the difference between
them matters:

- *Apply* sends your changes to the node and puts them into effect
  immediately, live, in RAM. They are *not* written to flash: if the node
  reboots or loses power, your changes are gone and it comes back on whatever
  was last saved (or the compiled-in defaults, if nothing was ever saved).
- *Save* does the same live apply and then writes the whole configuration to
  `/config.json` in the node's flash filesystem, so it survives a power cycle.

That ordering is deliberate. A setting that makes the node unreachable, a WiFi
change most of all, is one power cycle away from recovery as long as you have
not saved it.

The configuration is one JSON document, and a partial update merges rather than
replaces. Changing something on the *Follow* page cannot clobber your WiFi
credentials, and vice versa. v1's per-feature EEPROM records are gone.

*Workflow:* use *Apply* while you are experimenting. Try a slot, bench-test it,
tweak, repeat, with no commitment. Once you are happy, hit *Save* once to lock
it in. If you only ever hit Apply, you will re-enter your settings after every
reboot.

### Settings that need a reboot

Most changes, the whole Follow block included, take effect the moment you press
*Apply*. Three do not, and the UI raises a *Reboot required* banner when you
make one:

- The *group passphrase*. It only reaches the cipher at boot. Rekeying a live
  node would strand it mid-flight from every peer that has not been changed
  yet, which is a worse failure than waiting for a reboot.
- Enabling a radio that was switched off when the node booted. That radio's
  driver was never constructed, so there is nothing to switch on. Turning a
  radio *off* always works live.
- WiFi mode, SSID and passwords.

A factory reset (*Settings > Factory reset*) restores the compile-time defaults
and writes them straight to flash, but deliberately does not reboot the node,
so you can set a few fields before the radios come back up on a different
group.

---

## 5. Flying it: engaging Follow Mode

1. Confirm both aircraft are powered on, share a group passphrase, and can see
   each other: each should appear in the other's peer table on the *Dashboard*
   with a position that updates.
2. On the follower, arm as normal.
3. Flip the switch you assigned in §2.1 to activate `NAV POSHOLD` plus
   `GCS NAV` plus, for rotorcraft, `HEADING HOLD` together.
4. That is the entire trigger. As soon as `GCS NAV` goes active, the *Follow*
   page's *Live* card should show the lock state move `IDLE` to `ACQUIRING` to
   `LOCKED`, typically within a second or two once a followable peer is
   visible, and the follower should begin flying toward its configured slot.
5. To hand control back to yourself, flip the switch off. This immediately
   stops FF sending target updates and drops the lock. INAV's normal stick
   control, or whatever mode you switch to, takes over exactly as it would when
   leaving any other nav mode.

*First flight recommendation:* do not discover your configured slot's behavior
for the first time in the air. Bench-test it first (see below, and the
bench and HITL guides in `docs/explainers/`), and on the first real flight use
a generous, low-risk slot such as the chase-high default, trailing well behind
and above, at low leader speed, with the follow switch within easy reach the
entire time.

*Screenshot placeholder:* *[FPV goggles, normal formation flight, follower
tracking the leader at the configured slot]*

### Testing Follow Mode with only one aircraft

You do not need a second aircraft to exercise the slot geometry, the heading
mode, or the RC trim. The firmware can manufacture peers itself, from the
*Simulator* page in the sidebar.

A simulated peer is not a shortcut past the radio stack. Its positions are
encoded into real protocol packets, encrypted with the real group key, and
pushed through the same receive path real RF takes. The peer table, Follow and
the MSP radar output cannot tell the difference. That is the point: it
exercises the real code.

1. On the *Simulator* page, switch *Simulator enabled* on. It is saved to flash
   immediately and on purpose: a node that boots with simulated traffic running
   and nothing to say so is exactly the trap this setting guards against. While
   it is on, every page carries an unmissable banner.
2. Fill in the *Add a peer* form and press *Add peer*. The latitude and
   longitude are pre-filled from this node's own position if it has a fix, so
   you are not typing coordinates by hand. Pick a *Mode*:

| Mode | Path |
|---|---|
| `static` | Sits at the given position, reporting the given course and zero speed. |
| `line` | Departs from the given position on the given course at the given speed and never turns. |
| `circle` | Orbits the given position at *Radius* and the given speed. |
| `hex` | Flies a closed hexagon of side *Radius* centered on the given position, climbing to a peak 80 m above its start at the half-way vertex and descending back by the time the loop closes. |

Use `hex` for a real tracking test. It is the awkward one and therefore the
useful one: the heading changes in steps at each vertex, the altitude ramps up
and back down, and the loop closes, so the follower has to cope with all three.
A 150 m side at 15 m/s is a reasonable starting point.

3. Watch the *Follow* page's *Live* card. The simulated leader should appear,
   get locked onto, and the *Target* position should update as it moves around
   its path. Compare the reported target against where you expect the slot to
   be, given the leader's position and course.

Up to 4 simulated peers can run at once. Remove one with the button next to it,
or *Clear all*; switching the simulator off clears them too.

*Safety:* this is a bench-test tool. Props off. Only arm if you fully
understand what you are doing and why. A simulated leader is not a real
aircraft, and neither the peer table nor the flight controller has any way to
know that.

For the full walkthrough, including what to check and in what order, see
[`docs/explainers/bench-testing-follow-mode.md`](explainers/bench-testing-follow-mode.md).

---

## 6. Showing Follow status on your OSD (GVARs) [Optional]

FF's web UI is not something you can see while flying goggles-in. To get
follow-state feedback directly on your OSD, FF can write small status codes
into INAV *Global Variables* (GVARs), which you then turn into on-screen text
using INAV's own Programming Framework (Logic Conditions plus Custom OSD
Elements). FF only ever writes a number; INAV does the rest.

*Requires INAV 9.0.0 or later* on the follower flight controller.
`MSP2_INAV_SET_GVAR` does not exist before that. On older or non-INAV firmware
FF simply never sends anything: the feature stays inert, not broken.

### 6.1 Enable it on the Follow page

In the *GVAR* card, set:

- *Status GVAR*: which GVAR slot (`0` to `7`, or Disabled) carries the primary
  lock-state code.
- *Condition GVAR*: which GVAR slot carries a secondary condition code
  (currently: the altitude floor actively clamping, the target being too far,
  or an RC-related condition, §7). It has to be a different slot from every
  other GVAR index you assign; the UI and the firmware both refuse a
  collision.

Leave either Disabled (the default) if you do not want that indicator. Zero MSP
traffic is sent for a disabled slot.

| GVAR value (status) | Meaning |
|---|---|
| `0` | Follow gate inactive, nothing to show |
| `1` | `ACQUIRING`, searching for a leader to lock onto |
| `2` | `LOCKED`, tracking normally |
| `3` | `LOCKED_HOLDING`, leader telemetry stale or lost, holding position |

| GVAR value (condition) | Meaning |
|---|---|
| `0` | No condition active |
| `1` | Altitude floor is actively clamping the commanded altitude |
| `2` | Target too far from this craft, so Follow paused rather than chasing across an unbounded distance (see *Max target distance*, §3) |
| `3` | RC-driven slot is frozen at its last safe position, or the pre-arm RC check failed (§7) |

There is no code for "peer identity lost". v1 had one, because a slot id could
be reassigned to a different aircraft mid-flight. A UID cannot, so the
condition it reported cannot arise.

### 6.2 One-time INAV CLI setup

Pick your two GVAR indices first (whatever you entered in §6.1; the snippets
below use `<STATUS_GVAR_INDEX>` and `<CONDITION_FLAGS_GVAR_INDEX>` as
placeholders, so substitute your actual numbers before pasting). Paste the
whole block into Configurator's *CLI* tab, then run `save`.

```
logic 0 1 -1 1 5 <STATUS_GVAR_INDEX> 0 1 0
logic 1 1 -1 1 5 <STATUS_GVAR_INDEX> 0 2 0
logic 2 1 -1 1 5 <STATUS_GVAR_INDEX> 0 3 0
logic 3 1 -1 1 5 <CONDITION_FLAGS_GVAR_INDEX> 0 1 0
logic 4 1 -1 1 5 <CONDITION_FLAGS_GVAR_INDEX> 0 2 0
logic 5 1 -1 1 5 <CONDITION_FLAGS_GVAR_INDEX> 0 3 0
osd_custom_elements 0 1 0 0 0 0 0 2 0 "SEARCHING"
osd_custom_elements 1 1 0 0 0 0 0 2 1 "LOCKED"
osd_custom_elements 2 1 0 0 0 0 0 2 2 "HOLD LOST"
osd_custom_elements 3 1 0 0 0 0 0 2 3 "ALT FLOOR"
osd_custom_elements 4 1 0 0 0 0 0 2 4 "TOO FAR"
osd_custom_elements 5 1 0 0 0 0 0 2 5 "BAD RC"
save
```

What this does:

- The six `logic` lines each define a Logic Condition that evaluates "does the
  status GVAR (first three) or condition GVAR (last three) currently equal this
  code".
- The six `osd_custom_elements` lines each define a fixed piece of OSD text,
  visible only when its matching Logic Condition is true. Text is capped at 16
  characters and INAV auto-uppercases it regardless of how you type it here.
  Edit the quoted strings to whatever wording you prefer.
- Status code `0` (gate inactive) and condition code `0` (no condition active)
  deliberately have no matching element, so the OSD stays clean on flights
  where you never engage Follow Mode.

This example uses Logic Condition slots `0` to `5` and Custom OSD Element slots
`0` to `5`. If any of those are already used by something else on your
aircraft, use free slots instead and adjust each `osd_custom_elements` line's
visibility value (the number just before the quoted text, which names the Logic
Condition) to match.

### 6.3 Place the elements on your OSD layout

`osd_custom_elements` only *defines* the elements; it does not position them.
In Configurator's *OSD* tab the elements you just created appear in the item
list as `CUSTOM ELEMENT 1` through `6`. Drag each onto the OSD preview where
you want it.

### 6.4 Verify

With the follower connected and Follow Mode engaged, watch the OSD (in
Configurator's OSD preview, or in goggles) as you cycle through follow states:
engage the switch, let it lock, then stop the leader's telemetry, or move out
of radio range, to see `HOLD LOST` appear. On the bench, deleting the simulated
peer does the same thing on demand. Exactly one primary indicator should ever
be visible at a time, and with the gate off, none.

*Screenshot placeholders:*

- *[FPV goggles, OSD showing "SEARCHING"]*
- *[FPV goggles, OSD showing "LOCKED"]*
- *[FPV goggles, OSD showing "HOLD LOST"]*
- *[FPV goggles, OSD showing "ALT FLOOR" (or the RC-frozen condition) alongside a primary state]*

### 6.5 Raw coordinate debug GVARs

Separate from the status codes above, the *Debug GVARs* toggle on the *Follow*
page writes the commanded target's position and heading into four fixed GVAR
slots every cycle, for bench-testing in the goggles. It is a RAM-only toggle:
never persisted, always off again after a reboot.

| GVAR index | Value |
|---|---|
| `0` | North offset from the follower's own position, in cm (positive = target is north of you) |
| `1` | East offset from the follower's own position, in cm (positive = target is east of you) |
| `2` | Commanded altitude, home-relative, in cm |
| `3` | Commanded heading, absolute compass degrees (`0` to `359`) |

Indices `0` and `1` are a north/east offset from the follower's own position,
not absolute lat/lon. INAV's Custom OSD Elements cannot display absolute
lat/lon anyway: raw GPS degrees x 1e7 is a 9 or 10 digit number, but the widest
built-in numeric OSD part type clamps its display to 5 digits (+/-99999),
independent of the GVAR's own read/write range. A north/east offset in cm is
both small enough to fit that display and directly meaningful at a glance. A
"Behind 15 m" slot should read back as roughly -1500 on whichever of
North/East corresponds to the leader's direction of travel.

INAV clamps every GVAR write to that slot's configured range, and the default
range is -32768 to 32767. That is fine for indices `2` and `3`, and usually
fine for `0` and `1` too, since a follow slot's gap is rarely more than a few
hundred meters. If you are bench-testing with an unusually large offset and see
a value pinned at exactly 32767 or -32768, widen that slot's range:

```
gvar 0 0 -2000000000 2000000000
gvar 1 0 -2000000000 2000000000
gvar 3 0 -2000000000 2000000000
save
```

(`gvar` takes four arguments, `index default min max`, not three. The `default`
here is `0`, INAV's own default for an unconfigured GVAR.)

---

## 7. Trimming the slot live with RC channels [Optional]

Landing, connecting to FF's web UI, editing a slot and taking off again is slow
if you just want to nudge the formation gap mid-flight. The *RC axis control*
card lets you assign an ordinary RC channel to each axis so you can adjust the
slot live from your transmitter while Follow Mode is engaged. Assign the
channel to a slider or a rotary knob so you can move it smoothly.

### 7.1 What assigning a channel actually does

Once a channel is assigned to an axis, that axis's configured gap stops being a
fixed point and becomes a live-adjustable range, from the negative of that gap
to the positive of that gap:

- Channel centered (1500 us): the slot sits at the center of that axis, offset
  0.
- Channel at full deflection one way (2000 us): the slot sits at the full
  *positive* configured gap for that axis.
- Channel at full deflection the other way (1000 us): the slot sits at the full
  *negative* configured gap.

Concretely: if you configure "Behind, 15 m" and assign a channel to the fore/aft
axis, you are really configuring "this slot moves live between 15 m ahead and
15 m behind". The "Behind" label you picked becomes the *maximum in that
direction*, not a fixed position, the moment a channel is assigned.

An axis left Disabled (the default) is completely unaffected. It stays exactly
at its configured fixed value.

### 7.2 Setting it up

In the *RC axis control* card, set *Fore / aft channel*, *Left / right channel*
and *Up / down channel* to whichever RC channel number (1 to 16, as reported by
your receiver and flight controller) you want driving that axis, or leave it
Disabled. Each axis needs a different channel, and none of them may be the same
as the autothrottle arm channel (§10). The UI and the firmware both reject a
collision.

If you assign a channel to an axis whose configured gap is 0 m, that channel
has no effect, because there is no range to move within.

### 7.3 Safety: the slot will not fly straight through the leader

RC input can, in principle, drive all three axes to 0, 0, 0 at once: the same
degenerate "on top of the leader" position the safety bounds forbid for the
static configuration. FF guards against this live:

- Any candidate position that violates *Min separation* or *Min vertical
  separation* is rejected outright for that cycle.
- Beyond that, if your input would cause the slot to *cross* from one side of
  the leader to the other (for example sweeping the vertical channel from fully
  below to fully above while horizontal is centered) while the other two axes
  do not yet provide enough separation on their own, that crossing is blocked
  too. It would otherwise mean the commanded position passes directly through
  the leader's position for an instant.

When either of these trips, the slot *freezes* at the last position it was
safely holding. It stops responding to further channel movement in the unsafe
direction until you either move that channel back to a safe combination or
widen one of the other RC-assigned axes past *Min separation* first, the same
way a real formation pass has to route around another aircraft rather than
through it. The *Live* card says "RC slot frozen" while this is active, and the
condition GVAR (§6) reads `3`.

### 7.4 Pre-arm check

Because the very first "safe position" the freeze logic knows about is your
static configured slot, if your transmitter is already sitting somewhere that
disagrees with that slot the instant you engage Follow Mode, you can end up
frozen away from your actual channel position with no warning beyond the OSD
condition code.

To catch this before it matters, FF continuously checks, *while the aircraft is
disarmed only*, whether your current channel positions would produce a safe
slot if Follow Mode engaged right now, and whether they reproduce your
configured static default on every RC-assigned axis. If either check fails, the
*Live* card shows a pre-arm warning and displays the candidate offset next to
the live one, so you can compare the two.

*This is advisory only. It never blocks arming.* But heed it: the check
requires holding each RC-assigned axis at full deflection toward your
configured default's sign. A centered channel, which feels like the neutral
position, will *not* clear the check if your configured default for that axis
is nonzero. That is intentional. It forces you to confirm channel position
deliberately before arming rather than assuming center is safe. Get your
channel where the warning wants it before arming and engaging Follow, and this
scenario never comes up.

---

## 8. Troubleshooting

| Symptom | Likely cause |
|---|---|
| The leader never appears in the peer table at all | The two nodes have different group passphrases, so neither can decode the other. Check *Settings > Node > Security* on both, *Save*, and reboot both. A climbing `bad_tag` count on the Dashboard is the giveaway: frames are arriving and failing authentication. |
| The leader appears in the peer table but Follow stays `ACQUIRING` | The leader is beaconing without a valid GPS fix, or its last position is older than *Peer timeout*. A peer without a fix is never followable. Check the leader has 3D fix. |
| Flipping the switch does nothing, `Live` stays `IDLE` | `NAV POSHOLD` plus `GCS NAV` are not both actually active. Check INAV Configurator's Status tab, not just your transmitter switch position. The *Live* card's *Trigger gate* row says what FF sees. |
| `Live` shows `ACQUIRING` and never moves to `LOCKED` | No followable peer, or *Target peer* is pinned to a UID that is not currently transmitting a fix. Clear the field to fall back to nearest, or pick a UID from the peer buttons. |
| `Live` shows `LOCKED_HOLDING` | The locked leader's telemetry went stale (radio range, or the leader stopped broadcasting, or it lost GPS fix). The follower is intentionally holding position, not searching for a new target. See the peer-locking note in §3. |
| Follow will not accept your slot, with an "unsafe slot" error | Your slot values violate *Min separation* or *Min vertical separation*. Widen the gap, or reduce *Min separation* if that is genuinely appropriate for your aircraft size. |
| Follower holds well below or above where you expected | Check *Altitude floor*. If the leader's altitude, or your vertical offset, would put the follower below that floor, altitude is clamped up to it. That is intentional, not a bug. |
| Nose does not point where the Heading setting says it should | `HEADING HOLD` is not active on the follower. See the note under [Heading](#heading). It is a separate switch from `NAV POSHOLD` and `GCS NAV`. |
| RC channel does not move the slot at all | Either the axis is still Disabled, or its configured gap is 0 m, so there is no range to move within. |
| RC channel stops responding partway through its travel | You have hit the sign-lock freeze. See §7.3. Center the channel, or widen another RC-assigned axis first. |
| Settings revert after a power cycle | You used *Apply* but never *Save*. See §4. |
| A change appears to do nothing, and the UI shows *Reboot required* | Group passphrase changes, and switching on a radio that was off at boot, only take effect at boot. See §4. |
| Everything looks right, but no other aircraft ever sees this one | *Listen only* is switched on under *Settings > Node*. The node tracks peers but never transmits. |

If none of the above explains what you are seeing, the *Live* card's *Locked
peer* and *Target* rows are the most useful live diagnostic. Compare the
reported target lat/lon/altitude against where you would expect the slot to be,
given the leader's current position and course. The *Radios* page's per-radio
counters and the frame log are the next place to look if the problem is that
frames are not arriving.

---

## 9. Slot geometry diagram

The follow slot is defined relative to the *leader's current course*, not to
compass directions. It rotates with the leader. "Behind" always means behind
wherever the leader is currently pointed, not south, and not whatever direction
the leader happened to be facing when you configured the slot.

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

|  | UI label | Raw offset sign | Axis |
|---|---|---|---|
| Fore / aft (`ofsLongM`) | Ahead | positive (+) | along the leader's direction of travel |
| Fore / aft (`ofsLongM`) | Behind | negative (-) | " |
| Left / right (`ofsLatM`) | Right | positive (+) | perpendicular to travel, 90 deg clockwise |
| Left / right (`ofsLatM`) | Left | negative (-) | " |
| Up / down (`ofsVertM`) | Above | positive (+) | straight up |
| Up / down (`ofsVertM`) | Below | negative (-) | straight down |

For the full worked math behind this projection (how a track-relative offset
becomes an absolute lat/lon), see
[`docs/explainers/follow-target-geometry.md`](explainers/follow-target-geometry.md).
That is implementation detail aimed at developers, not required reading to fly
this feature.

---

## 10. Speed Autothrottle (fixed wing) [Optional]

*This section only applies to a fixed-wing follower.* It assumes your fixed
wing already flies `NAV POSHOLD` plus `GCS NAV` acceptably well on its own
before you add speed control on top. If it does not, fix that first. Everything
below only ever adjusts throttle, never position.

### 10.1 What this adds

Follow Mode by itself only ever commands a *position*. It has no opinion on how
fast the follower gets there. On a fixed wing that is often too sluggish to
hold a tight slot when the leader speeds up, slows down or turns: FF's position
stream does not change, but the follower's own airspeed does. Speed Autothrottle
closes that gap by having FF compute a target ground speed every cycle,
matching the leader's own speed and then nudging it up or down depending on
whether the follower is lagging behind or running ahead of its slot, and
writing that number into an INAV Global Variable that drives a Programming
Framework throttle override on the follower's flight controller.

FF only ever writes two numbers: the target speed, and a 0/1 flag saying
whether autothrottle should be active right now. INAV does the rest, and this
section is that one-time INAV setup, mirroring §6's OSD GVAR setup.

*Requires INAV 9.0.0 or later* (for the GVAR writes) and an *airplane mixer* on
the follower FC, which is checked automatically (§10.3).

### 10.2 Enable it on the Follow page

In the *Autothrottle* card, set:

- *Target speed GVAR*: which GVAR slot (`0` to `7`, or Disabled) carries the
  commanded ground-speed setpoint, in cm/s.
- *Engage GVAR*: which GVAR slot carries the 0/1 engage flag. It has to differ
  from the target speed slot and from the §6 status and condition slots.
- *Arm channel* (optional): an RC channel to use as a hardware autothrottle
  on/off switch, independent of the follow switch itself. Leave it Disabled if
  you want autothrottle to run automatically whenever Follow Mode is locked on
  a fixed wing, with no extra switch. If you assign a channel, autothrottle is
  only active while that channel's pulse width falls inside *Arm range min* and
  *Arm range max* (us). A range, not a single threshold, so it can describe a
  2-position switch (a wide range covering the whole high half of travel), a
  3-position switch, or one specific position on a 6-position switch, whichever
  you have physically wired. The channel also has to differ from the RC axis
  channels in §7.
- *Min speed* and *Max speed*: hard floor and ceiling on the commanded speed,
  in m/s. Both default to 0, and both *must* be set explicitly once an arm
  channel is assigned: the firmware refuses to accept an arm channel alongside
  the 0 defaults, so you cannot arm the feature without having set a speed
  range. No default is provided because a sane range varies enormously between
  airframes. *Min speed is this feature's only stall-safety mechanism*: there
  is no dynamic sink-rate or rescue correction. Set it comfortably above this
  airframe's actual stall speed. Roughly a third above stall is a reasonable
  starting point, not a validated number for your aircraft. With the arm
  channel Disabled these two fields are ignored and can stay at 0.
- *Slot-lag accel*: maximum closing acceleration or deceleration, in cm/s^2,
  used to speed up or slow down beyond the leader's raw ground speed to correct
  for lagging or leading the slot. It follows a kinematic braking curve
  (`v = sqrt(2 · accel · distance)`), so it ramps up quickly when the follower
  is far from its slot and tapers off smoothly as it arrives, matching the
  leader's speed exactly once in slot, rather than a constant-rate correction
  that is equally aggressive at any distance. `0` (the default) means pure
  feedforward: mirror the leader's speed, no correction. Leave it at 0 until
  you have flown the feature once and have a specific lag or lead behavior you
  want to tighten up. Higher values catch up faster but brake harder right
  before reaching the slot.

The *Live* card reports the FC's mixer platform as soon as the flight
controller answers. The firmware refuses to engage on anything other than an
airplane mixer regardless of what you configure (§10.3's real gate).

### 10.3 The three-way engage gate

Autothrottle only ever engages when all three of these are true, checked fresh
every cycle with no latching:

1. Follow Mode has a leader actively *locked*. `ACQUIRING`, `LOCKED_HOLDING`
   and gate-inactive all count as not engaged.
2. The follower FC is reporting an *airplane* mixer, read live from INAV over
   MSP rather than configured.
3. The *arm channel*, if you assigned one, is in its armed range. Leave it
   Disabled and this condition is always satisfied.

Losing any one of the three drops the engage flag to `0` and the aircraft falls
through to INAV's own regular `NAV POSHOLD` speed behavior. It never holds a
stale setpoint. This is what lets you run ordinary position-only follow on a
fixed wing with autothrottle switched off entirely, just by flipping the arm
channel, with no web UI trip required.

### 10.4 One-time INAV CLI setup

Pick your two GVAR indices first (§10.2). The snippet below uses `<E>` for the
engage GVAR index and `<T>` for the target speed GVAR index. *Substitute your
actual numbers (0 to 7) before pasting; do not paste `<E>` and `<T>`
literally.* Paste the whole block into Configurator's *CLI* tab, then run
`save`.

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

- `logic 33` reads your engage GVAR. This is the *only* Logic Condition INAV
  needs to know about FF's engage decision. Everything upstream of it (lock
  state, airframe check, arm switch) already happened on the FF side before the
  GVAR was written.
- `logic 36` is the "TARGET" OSD readout's unit conversion. Cosmetic, not part
  of the control loop.
- `logic 39` to `46` are the throttle-output chain: take Programmable PID 3's
  output, offset and scale it into a servo-pulse range, and force-write it to
  the throttle channel. This clamps to 1250 to 1800 us, which is an actuator
  and hardware range, not a speed bound. It has nothing to do with *Min speed*
  and *Max speed*, which are already fully resolved before the GVAR is written.
  It limits the throttle to roughly 25% to 80%, so you do not drop throttle low
  enough to risk ESC desync when it throttles up quickly, and do not sit pegged
  at 100% for a long time. Change those values if you are comfortable pushing
  your craft further.
- `pid 3` is the actual speed-hold PID: the setpoint reads your target speed
  GVAR directly, the measurement reads the flight controller's own live ground
  speed. This is the loop that turns "FF wants X m/s" into a real throttle
  position.
- The two `osd_custom_elements` lines add a throttle-percent readout and the
  "TARGET" speed readout to your OSD element list. Drag them onto your OSD
  layout in Configurator's *OSD* tab the same way §6.3 describes.

This example uses Logic Condition slots `33`, `36` and `39` to `46`,
Programmable PID slot `3`, and Custom OSD Element slots `0` and `1`. If any of
those are already used on your aircraft, use free slots and update the block's
internal cross-references (the `33`s in the later lines refer back to
`logic 33`).

#### Fallback, if your INAV build rejects the `pid 3` line above

*Not fully tested. Use at your own risk.*

A small number of INAV builds may not accept a flight-telemetry (ground speed)
measurement source directly on a Programmable PID line. If `save` fails, or
PID3's live measurement value in Configurator does not track your GPS ground
speed, use this fallback instead. Only the `pid 3` line changes, plus five
extra Logic Conditions feeding a scratch GVAR (`GVAR1` below is illustrative;
pick any index that does not collide with `<E>`, `<T>`, or any other
FF-assigned GVAR):

```
logic 0 1 -1 1 2 31 0 1 0
logic 1 1 0 2 2 9 0 1000 0
logic 2 1 0 13 4 1 4 3 0
logic 3 1 -1 1 2 17 0 0 0
logic 4 1 2 14 2 9 0 0 0
logic 50 1 33 18 0 1 4 4 0

pid 3 1 5 <T> 5 1 800 550 80 400
```

This fallback uses one additional GVAR beyond the primary block, worth
remembering if you have other FF GVAR features (§6, debug GVARs) enabled
simultaneously and are running low on INAV's 8-GVAR budget.

### 10.5 Verify

With the follower on the ground (props off) or in the air on an airplane mixer:

1. Confirm the *Follow* page's *Autothrottle* readout shows engaged once Follow
   Mode is locked onto a leader, real or [simulated](#testing-follow-mode-with-only-one-aircraft),
   with a target speed roughly matching the leader's ground speed.
2. In Configurator's real-time monitor, confirm PID3's live setpoint tracks
   your target speed GVAR's value, and its measurement tracks the FC's own GPS
   ground speed.
3. Flip your arm channel off, if you assigned one. Confirm the engaged
   indicator drops and PID3's setpoint stops updating within a second or two.
   Flip it back on and confirm it resumes immediately, with no re-lock needed.
4. If your follower FC is not an airplane mixer, confirm it stays disengaged
   regardless of lock state. That is the airframe gate from §10.3 working as
   intended, not a bug.

*Screenshot placeholder:* *[FPV goggles, OSD showing throttle % and TARGET
speed readouts while autothrottle is engaged]*

---

## 11. Configuring Follow over the API

Everything on the *Follow* page is a view over the node's JSON API, which is
documented in full in [`docs/v2-web-api.md`](v2-web-api.md). That document is
the contract; nothing here overrides it. This section is only the Follow
specifics, for anyone scripting config changes or polling status instead of
using the web UI. The v1 `/followmanager/*` endpoints are gone.

What you need to know:

- `GET /api/config` returns the whole configuration. Follow's settings are the
  `follow` object inside it, with the field names listed below.
- `POST /api/config` takes a *partial* config and applies only the keys
  present, in RAM, immediately. Posting `{"follow": {"ofsLongM": -20}}` changes
  one field and touches nothing else. If validation fails, nothing changes and
  the response is `400` with a plain-text message naming the offending field,
  which is the same string the UI shows.
- `POST /api/config/save` (empty body) persists the live configuration to
  flash. Without it your change is gone at the next power cycle.
- `GET /api/status` includes a `follow` object with the live state: lock state,
  gate, locked UID and name, last target, GVAR values, live and pre-arm
  offsets, and the autothrottle state. Fields that have never been computed are
  *absent* rather than zero, because zero is a legitimate offset and must not
  be confused with "no data".

```bash
# Move the slot 20 m behind the leader and make it stick
curl -X POST -H 'Content-Type: application/json' \
     -d '{"follow": {"ofsLongM": -20}}' http://192.168.4.1/api/config
curl -X POST http://192.168.4.1/api/config/save

# Watch the lock state
curl -s http://192.168.4.1/api/status
```

### The `follow` config block

| Field | Type | Meaning |
|---|---|---|
| `ofsLongM` | number | Longitudinal offset, meters (+ahead / -behind), §3 |
| `ofsLatM` | number | Lateral offset, meters (+right / -left) |
| `ofsVertM` | number | Vertical offset, meters (+above / -below) |
| `triggerMode` | string | `"GCSNAV"` or `"AUX"`. Read-only: compiled in, ignored on the way back in |
| `targetUid` | number | Peer UID to pin to, or `0` for "nearest followable peer" |
| `emitHz` | number | Control cycle and target-send rate, Hz. Must be greater than 0 |
| `peerTimeoutMs` | number | Leader position staleness timeout, ms. Must be greater than 0 |
| `minSepM` | number | Minimum allowed 3D separation from the leader, meters |
| `minVSepM` | number | Minimum vertical separation when the slot is stacked, meters |
| `maxTargetDistM` | number | Refuse to command a target farther than this from the follower, meters. Must be greater than 0 |
| `minAltM` | number | Altitude floor, home-relative, meters |
| `minCourseSpeed` | number | Below this leader ground speed (m/s), freeze slot orientation at the last trusted course |
| `headingMode` | string | `"OFF"`, `"COURSE"`, `"POINT_LEADER"`, `"FIXED"` or `"COURSE_RELATIVE"`, §3 |
| `headingDeg` | number | Degrees. Absolute heading for `FIXED`, offset for `COURSE_RELATIVE` |
| `statusGvarIndex` | number | GVAR index `0` to `7` for the status code, or `-1` for disabled, §6 |
| `conditionFlagsGvarIndex` | number | GVAR index for the condition code, or `-1` |
| `rcLongChannel` | number | 1-based RC channel (1 to 16) driving the fore/aft axis, or `-1`, §7 |
| `rcLatChannel` | number | RC channel driving the left/right axis, or `-1` |
| `rcVertChannel` | number | RC channel driving the up/down axis, or `-1` |
| `targetSpeedGvarIndex` | number | GVAR index for the autothrottle speed setpoint, or `-1`, §10 |
| `autothrottleEngageGvarIndex` | number | GVAR index for the autothrottle engage flag, or `-1` |
| `autothrottleEnableRcChannel` | number | RC channel used as the autothrottle arm switch, or `-1` for always armed |
| `autothrottleEnableMinThresholdUs` | number | Lower bound (us) of the arm switch's armed range |
| `autothrottleEnableMaxThresholdUs` | number | Upper bound (us) of the armed range. Must be greater than the lower bound |
| `speedCorrectionAccelCmS2` | number | Slot-lag correction accel, cm/s^2. `0` is pure feedforward |
| `minTargetSpeedMps` | number | Lower clamp on the commanded autothrottle speed, m/s. Must be greater than 0 once `autothrottleEnableRcChannel` is set |
| `maxTargetSpeedMps` | number | Upper clamp, m/s. Must be greater than `minTargetSpeedMps` once an arm channel is set |
| `debug` | boolean | RAM-only debug-GVAR toggle, §6.5. Never persisted; always `false` after a reboot |

Two things to watch:

- `targetUid` is a plain JSON *number*, unlike every UID elsewhere in the API,
  which is a lower-case 8-character hex *string*. The web UI does the
  conversion for you; a script has to do it itself. `0x1a2b3c4d` is
  `439041101`.
- All GVAR indices must be unique across status, condition, target speed and
  engage, and all RC channels unique across the three axes and the autothrottle
  arm channel. The firmware rejects a collision outright, with the message
  naming which rule you broke.

Example `follow` block, at the shipped defaults:

```json
{
  "ofsLongM": -15,
  "ofsLatM": 0,
  "ofsVertM": 10,
  "triggerMode": "GCSNAV",
  "targetUid": 0,
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
