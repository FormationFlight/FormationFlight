# Decoded INAV Programming: Ground-Speed-Hold Autothrottle

This is a plain-English / pseudocode translation of a raw INAV `logic` / `pid` /
`osd_custom_elements` CLI dump. It is **not** part of FormationFlight — it's a
standalone INAV Programming Framework script (a fixed-wing autothrottle that
holds a target speed via a Programmable PID, with a switch-gated
engage/disengage state machine and OSD readouts). Decoded against INAV's
`master` source (`programming/logic_condition.h`, `io/osd/custom_elements.h`,
`fc/cli.c`) for field order and enum values.

**This version has been modified from the original design.** The script was
originally written around a true pitot airspeed sensor (with GPS 3D speed as
a secondary fallback) and a "3D speed > airspeed" sink-rate proxy. Without a
pitot tube installed, that proxy is meaningless — `3D speed` is always ≥
`ground speed` whenever there's *any* vertical movement (climbing or
descending), so it would falsely trigger the correction latch constantly
(this is why `"OVERRIDING "` was showing on the OSD all the time). The
pseudocode and CLI below reflect the **ground-speed variant**: the pitot
reading is replaced with GPS ground speed throughout, and the sink-rate
proxy is replaced with a direct vertical-speed check. The original
airspeed/GPS-fallback selection logic (`LC47`–`LC52`) is disabled, since both
branches would otherwise be GPS-derived and the distinction is moot.

Variable names below are my own invention — INAV's raw dump only has numeric
Logic Condition (LC) IDs. A mapping table further down lets you cross-check
each name against the original LC number. The full modified CLI command
block is in the [Appendix](#appendix-full-cli-command-block-ground-speed-variant).

## Pseudocode

```python
# ============================================================
# Persistent state (survives across ticks — INAV re-evaluates
# all Logic Conditions every control-loop tick, but these act
# like latches/registers because of how they're wired)
# ============================================================
hasReachedFlyingSpeed  = False   # sticky latch, LC2
correctingForLowGroundSpeed = False # sticky latch, LC25
autothrottleEngaged    = False   # sticky latch, LC33 — THE master enable flag

targetGroundSpeed = 0   # GVAR0 — the PID setpoint, in cm/s
currentGroundSpeed = 0  # GVAR1 — the PID measurement, in cm/s

KMH_TO_CMS      = 28      # rough km/h -> cm/s conversion factor (27.78, rounded)
BASE_TARGET_CMS = 50 * KMH_TO_CMS   # = 1400 cm/s (~50 km/h) baseline target

SINK_RATE_THRESHOLD_CMS = -100   # trigger correction below -1 m/s vertical speed — tune to taste

# ============================================================
# Runs once per INAV control-loop tick
# ============================================================
def tick(rc, flight, pid3_output):

    # ---- 1. Basic sensor reads -----------------------------
    gpsValid       = flight.gps_fix_valid                      # LC0
    groundSpeedReading = flight.ground_speed                   # LC4 (was: flight.airspeed) — only refreshed while hasReachedFlyingSpeed
    verticalSpeed   = flight.vertical_speed                     # read directly in step 5, no longer via LC5
    gpsSpeed3D      = flight.speed_3d                           # LC5 — now unused downstream, kept enabled but harmless

    # ---- 2. "Has this flight actually gotten airborne and
    #         up to speed yet?" gate ---------------------------
    if gpsValid:
        groundSpeedFast = flight.ground_speed > 1000            # LC1, cm/s
    else:
        groundSpeedFast = STALE                                 # not re-evaluated

    isDisarmed = not flight.armed                                # LC3

    # STICKY: set on rising edge of groundSpeedFast,
    #         reset on rising edge of isDisarmed
    hasReachedFlyingSpeed = sticky(set=groundSpeedFast, reset=isDisarmed)   # LC2

    # ---- 3. Engage switch + abort conditions -----------------
    # (only re-armable once this flight has proven it can fly fast,
    #  and isn't still in the autolaunch/catapult phase)
    if hasReachedFlyingSpeed:
        notInAutolaunch = not flight.is_autolaunch               # LC53
    if notInAutolaunch:
        engageSwitchHigh = rc.channel[11] > 1480                 # LC27, a 2-position switch
    engageSwitchEdge = rising_edge(engageSwitchHigh, hold_ms=100) # LC28

    yawStickMoved = stick_delta(rc.channel[4], threshold=90)      # LC29 — pilot touched yaw/rudder
    isLanding      = flight.is_landing                            # LC30
    gpsInvalid     = not gpsValid                                  # LC12

    abortCondition = gpsInvalid or yawStickMoved or isLanding      # LC31 + LC32

    # Master latch: engages on switch flip, disengages automatically
    # on GPS loss / pilot yaw input / landing. Only updates while the
    # switch itself is physically ON (activator = engageSwitchHigh).
    if engageSwitchHigh:
        autothrottleEngaged = sticky(set=engageSwitchEdge, reset=abortCondition)  # LC33

    # ---- 4. Measurement source: ground speed, direct ----------
    # LC47-LC52 (pitot-vs-GPS-fallback selection) are disabled —
    # with no pitot, both branches would be GPS-derived anyway, so
    # the distinction from the original design no longer applies.
    if autothrottleEngaged:
        currentGroundSpeed = groundSpeedReading                        # LC50, GVAR1 = ground speed, unconditional

    # ---- 5. Sink-rate auto-correction --------------------------
    # If we're descending faster than the threshold AND ground speed
    # is below the baseline target, nudge the setpoint up. (Was:
    # "3D speed > airspeed" — meaningless without a pitot, since 3D
    # speed is always >= ground speed on any climb or descent.)
    if autothrottleEngaged:
        sinkRateExcessive = verticalSpeed < SINK_RATE_THRESHOLD_CMS     # LC21
        if sinkRateExcessive:
            groundSpeedBelowBase = groundSpeedReading < BASE_TARGET_CMS  # LC22
            if groundSpeedBelowBase:
                speedDeficit = BASE_TARGET_CMS - groundSpeedReading       # LC23
                targetGroundSpeed += speedDeficit                          # LC24 — GVAR_INC

        # Latches "actively correcting"; clears once ground speed climbs
        # back above baseline.
        groundSpeedRecovered = groundSpeedReading > BASE_TARGET_CMS       # LC26 (evaluated while latch set)
        correctingForLowGroundSpeed = sticky(set=<LC24 fired>,
                                              reset=groundSpeedRecovered)  # LC25

    # ---- 6. Pilot manual target-speed trim ----------------
    # Overrides the auto-corrected setpoint if the pilot dials in
    # a target via RC channel 12 (a knob/trim).
    if autothrottleEngaged and not correctingForLowGroundSpeed:
        pilotTrimRaw = rc.channel[12] - 1000                          # LC35, 0..1000
        if pilotTrimRaw != 0:
            pilotTrimScaled = map_output(pilotTrimRaw, 110)            # LC36 — exact INAV mapper range unverified
            if pilotTrimScaled != 0:
                pilotTargetCms = pilotTrimScaled * KMH_TO_CMS           # LC37 — likely km/h -> cm/s
                targetGroundSpeed = pilotTargetCms                       # LC38 — hard SET, overrides LC24's increment

    # ---- 7. PID -> throttle output ----------------------------
    if autothrottleEngaged:
        # Programmable PID #3: setpoint=targetGroundSpeed, measurement=currentGroundSpeed
        # P=800  I=550  D=80  FF=400
        pidOutput = pid3_output   # computed by INAV's PID engine from the above

        throttleRaw     = pidOutput + 3000                            # LC39
        throttleHalved  = throttleRaw / 2                               # LC40
        throttleClamped = clamp(throttleHalved, lo=1250, hi=1800)       # LC41/42/43, servo-pulse µs
        override_throttle(throttleClamped)                              # LC44 — actual actuation

        throttleDisplayPct = throttleClamped / 10 - 100                 # LC45/46, ~25-80 for OSD

    return {
        "autothrottleEngaged": autothrottleEngaged,
        "correctingForLowGroundSpeed": correctingForLowGroundSpeed,
        "pilotTrimScaled": pilotTrimScaled,
        "throttleDisplayPct": throttleDisplayPct,
    }


# ============================================================
# OSD (each element only rendered while its condition is true)
# ============================================================
def render_osd(state):
    if state.autothrottleEngaged:
        show(icon(171), icon(172), number(state.throttleDisplayPct, digits=2))

    if state.pilotTrimScaled:
        show("TARGET ", number(state.pilotTrimScaled, digits=2), icon(144))

    # Elements 2/3 (pitot-active / GPS-fallback-active icons) are
    # disabled — see the LC47-LC52 note above. Their visibility now
    # points at an always-zero GVAR so neither ever renders.

    if state.correctingForLowGroundSpeed:
        show("OVERRIDING ", icon(140), icon(144))
```

## Walkthrough

**Flying-speed gate.** The system won't even let you engage the mode until
the aircraft has proven it's airborne and moving (`ground speed > 10 m/s`)
at least once since arming — this is `hasReachedFlyingSpeed`, a latch that
only clears on disarm. That, plus "not in autolaunch," gates whether the
engage switch is even listened to, so you can't accidentally trigger
autothrottle during a hand-launch/catapult run.

**Engage switch.** RC channel 11 above 1480 arms the switch check; a rising
edge on that switch sets the master `autothrottleEngaged` latch. Three
things auto-disengage it without touching the switch: GPS fix lost, the
pilot moving the yaw stick (channel 4) more than a small threshold, or the
aircraft entering its landing state.

**Measurement source.** The PID's measurement input is GPS ground speed,
unconditionally, whenever the mode is engaged. In the original pitot-based
design this was a choice between a real airspeed sensor and a GPS fallback,
selected by flight mode/switch state; without a pitot both branches would
have been GPS-derived anyway, so that selection logic (`LC47`–`LC52`) is
disabled rather than kept around as dead weight.

**Sink-rate correction.** If vertical speed drops below the configured
threshold (default: descending faster than 1 m/s) *and* ground speed has
dropped below a ~50 km/h baseline, the target ground-speed setpoint
(`GVAR0`) is nudged upward by the deficit every tick, latching an "actively
correcting" flag until ground speed recovers above baseline. This replaces
the original "3D speed > airspeed" proxy, which — once ground speed stood in
for airspeed — would have fired on *any* climb or descent (3D speed is
always ≥ ground speed whenever vertical speed is nonzero), not just genuine
sink. A direct vertical-speed threshold is a more honest sink detector
regardless.

**Pilot trim override.** If the pilot dials RC channel 12 away from its
minimum, that value passes through INAV's `MAP_OUTPUT` function and gets
multiplied by 28 (the same km/h→cm/s conversion factor used to build the
1400 cm/s baseline), then **hard-overwrites** the target ground speed —
overriding the automatic sink-rate correction, not adding to it. This gate
only applies when not currently in the correction latch.

**PID → throttle.** A Programmable PID (bank 3, gains P=800/I=550/D=80/FF=400)
drives target vs. current ground speed to zero error. Its raw output is
offset by +3000, halved, then clamped into 1250–1800µs — a standard RC pulse
range — before being force-written onto the throttle channel via
`OVERRIDE_THROTTLE`. That means once engaged, the pilot's throttle stick is
ignored; the FC is fully flying the throttle itself.

**OSD.** Three active elements report state at a glance: an
engaged/throttle-% readout, a "TARGET" readout for pilot-trim mode, and an
"OVERRIDING" warning when the sink-rate correction has kicked in. The two
airspeed-source icons from the original design are now permanently hidden
(see appendix).

> **Tuning note:** ground speed is far more wind-sensitive than true
> airspeed — a headwind depresses ground speed even at a perfectly safe
> airspeed, which can trigger the sink-rate correction (and "OVERRIDING")
> more readily than the original pitot-based design would have. Consider
> lowering `BASE_TARGET_CMS` (`LC20`) if you're seeing frequent triggers
> into a headwind, or loosen `SINK_RATE_THRESHOLD_CMS` (`LC21`'s literal,
> currently -100) if it's firing on normal descents.

## LC-ID → variable name mapping

| LC ID | Variable | LC ID | Variable |
|---|---|---|---|
| 0 | gpsValid | 27 | engageSwitchHigh |
| 1 | groundSpeedFast | 28 | engageSwitchEdge |
| 2 | hasReachedFlyingSpeed | 29 | yawStickMoved |
| 3 | isDisarmed | 30 | isLanding |
| 4 | groundSpeedReading *(was airspeedReading)* | 31 | abortCondition (partial: gps/yaw) |
| 5 | gpsSpeed3D *(now unused downstream)* | 32 | abortCondition (+landing) |
| 12 | gpsInvalid | 33 | autothrottleEngaged |
| 20 | BASE_TARGET_CMS (1400) | 34 | notCorrecting |
| 21 | sinkRateExcessive *(was sinkingFasterThanAirspeed)* | 35 | pilotTrimRaw |
| 22 | groundSpeedBelowBase | 36 | pilotTrimScaled |
| 23 | speedDeficit | 37 | pilotTargetCms |
| 24 | targetGroundSpeed += (GVAR0 inc) | 38 | targetGroundSpeed = (GVAR0 set) |
| 25 | correctingForLowGroundSpeed | 39 | throttleRaw |
| 26 | groundSpeedRecovered | 40 | throttleHalved |
| — | — | 41–43 | throttleClamped |
| — | — | 44 | override_throttle() |
| 47 | *disabled* (was: poshold) | 45–46 | throttleDisplayPct |
| 48 | *disabled* (was: switchFullyHigh) | | |
| 49 | *disabled* (was: useRealAirspeed) | | |
| 50 | currentGroundSpeed (GVAR1), unconditional | | |
| 51/52 | *disabled* | | |
| 53 | notInAutolaunch | | |

## Confidence / caveats

- Field orders and enum names (`logicOperation_e`, `logicOperandType_e`,
  `logicFlightOperands_e`, OSD part/visibility types) are verified against
  INAV's `master` branch source.
- `MAP_OUTPUT`'s exact input/output range (the `110` parameter) isn't fully
  verified — I couldn't confirm its runtime scaling from the CLI dump alone,
  since INAV's value-mapper ranges are normally configured in Configurator's
  UI, not visible in this snippet. Treat `pilotTrimScaled`'s units as a
  best guess.
- The `50 × 28` / `× 28` reuse as a km/h→cm/s conversion factor is inferred
  from context (28 ≈ 27.78 cm/s per km/h), not confirmed by a comment in the
  original dump.
- INAV evaluates Logic Conditions in ID order every tick; a rule referencing
  a *higher*-numbered rule (e.g. `LC21`'s activator is `LC33`, defined later)
  sees that rule's value from the *previous* tick, not the current one. The
  pseudocode above presents everything as if synchronous within one tick —
  in reality there's a ~1-tick lag on those specific forward references,
  which is negligible at INAV's loop rate but worth knowing if you're
  debugging exact timing.
- `SINK_RATE_THRESHOLD_CMS` (`-100`, i.e. -1 m/s) is a starting-point value,
  not something recovered from the original dump — the original comparison
  didn't use vertical speed at all, so there's no "correct" threshold to
  decode. Tune it against your aircraft's normal descent rate.
- Disabling `LC47`–`LC49`/`LC51`/`LC52` by zeroing their `enabled` flag (as
  shown in the appendix) leaves their last-evaluated value frozen in place
  rather than resetting it — this only matters if something else still reads
  them, which nothing does after `LC50`'s rewire and the OSD-visibility
  change below.

## Appendix: full CLI command block (ground-speed variant)

Everything from scratch, ready to paste into INAV Configurator's CLI tab,
followed by `save`. Changes from the original airspeed-based dump: `LC4`
now reads `GROUND_SPEED` instead of `AIR_SPEED`; `LC21` is rewritten to a
direct vertical-speed threshold; `LC47`/`48`/`49`/`51`/`52` are disabled;
`LC50` is simplified to set `GVAR1` unconditionally once engaged; the two
airspeed-source OSD icons (elements `2`/`3`) have their visibility
repointed at GVAR index `7`, which nothing ever sets, so they stay
permanently hidden instead of freezing on a stale icon. **If GVAR index 7 is
already used for something else on your craft, pick a different unused
index (0–7) for both `osd_custom_elements` lines below.**

```
logic 0 1 -1 1 2 31 0 1 0
logic 1 1 0 2 2 9 0 1000 0
logic 2 1 0 13 4 1 4 3 0
logic 3 1 -1 1 2 17 0 0 0
logic 4 1 2 14 2 9 0 0 0
logic 5 1 2 14 2 10 0 0 0
logic 12 1 -1 1 2 31 0 0 0
logic 20 1 -1 16 0 50 0 28 0
logic 21 1 33 3 2 13 0 -100 0
logic 22 1 21 3 4 4 4 20 0
logic 23 1 22 15 4 20 4 4 0
logic 24 1 23 19 0 0 4 23 0
logic 25 1 27 13 4 24 4 26 0
logic 26 1 25 2 4 4 4 20 0
logic 27 1 53 2 1 11 0 1480 0
logic 28 1 -1 47 4 27 0 100 0
logic 29 1 -1 50 1 4 0 90 0
logic 30 1 -1 1 2 23 0 1 0
logic 31 1 -1 8 4 12 4 29 0
logic 32 1 -1 8 4 31 4 30 0
logic 33 1 27 13 4 28 4 32 0
logic 34 1 33 12 4 25 0 0 0
logic 35 1 34 15 1 12 0 1000 0
logic 36 1 35 37 4 35 0 110 0
logic 37 1 36 16 4 36 0 28 0
logic 38 1 37 18 0 0 4 37 0
logic 39 1 33 14 6 3 0 3000 0
logic 40 1 33 17 4 39 0 2 0
logic 41 1 33 43 0 1800 4 40 0
logic 42 1 33 44 0 1250 4 41 0
logic 43 1 33 44 4 41 4 42 0
logic 44 1 33 29 4 43 0 0 0
logic 45 1 33 17 4 43 0 10 0
logic 46 1 33 15 4 45 0 100 0
logic 47 0 -1 0 0 0 0 0 0
logic 48 0 -1 0 0 0 0 0 0
logic 49 0 -1 0 0 0 0 0 0
logic 50 1 33 18 0 1 4 4 0
logic 51 0 -1 0 0 0 0 0 0
logic 52 0 -1 0 0 0 0 0 0
logic 53 1 2 1 2 18 0 0 0

pid 3 1 5 0 5 1 800 550 80 400

osd_custom_elements 0 2 171 2 172 18 46 2 33 ""
osd_custom_elements 1 1 0 18 36 2 144 2 36 "TARGET "
osd_custom_elements 2 2 42 0 0 0 0 1 7 ""
osd_custom_elements 3 2 42 0 0 0 0 1 7 ""
osd_custom_elements 4 1 0 2 140 2 144 2 25 "OVERRIDING "

save
```

Lines unchanged from the original dump are included verbatim so this block
is a complete, standalone replacement — you don't need to merge it against
anything.
