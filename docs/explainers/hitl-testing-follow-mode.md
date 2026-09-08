# HITL: Real Flight Controller, Simulated Movement

[`bench-testing-follow-mode.md`](bench-testing-follow-mode.md) fakes
everything at fixed points — nothing moves, and the doc's own "what still
needs a real flight" section says so explicitly: it cannot show *whether the
follower's flight controller actually captures and holds the commanded
waypoint* — that's INAV's POSHOLD/nav-to-waypoint control loop, not
`FollowManager`'s. [`sitl-testing-follow-mode.md`](sitl-testing-follow-mode.md)
removes the physical aircraft entirely by running INAV as a PC binary, but
loses the real flight controller in the process.

This doc covers the option in between: keep a **real** flight controller —
real silicon, real firmware, unmodified — but replace its sensor inputs
(GPS, IMU, baro, mag) with simulated values fed over MSP, so its own
navigation code believes it's flying and reacts accordingly. This is
Hardware-In-The-Loop (HITL), and it directly answers the question bench
testing can't: does the follower FC's nav loop actually turn toward and
settle on the waypoint `FollowManager` is feeding it. FF itself stays real
hardware throughout, exactly as in the bench-testing doc.

## What makes this possible: INAV ships HITL support in normal firmware

This isn't a special build. `USE_SIMULATOR` is defined in
[`target/common.h:181`](https://github.com/iNavFlight/inav/blob/master/src/main/target/common.h),
which is included by essentially every INAV hardware target — the same
firmware you'd normally flash to a real FC already contains this code path.
(If you want to be certain for your specific board, check that your
target's `.h`/`CMakeLists.txt` doesn't `#undef` it — this is rare.)

The mechanism is the MSP2 command `MSP_SIMULATOR` (`0x201F`, defined in
`msp_protocol_v2_inav.h:62`), handled by `mspProcessSimulatorCommand()` /
`readMspSimulatorValues()` in
[`fc_msp.c:4356-4432`](https://github.com/iNavFlight/inav/blob/master/src/main/fc/fc_msp.c).
Sending it with the `HITL_ENABLE` flag set (and `HITL_SITL_MODE` **left
clear** — that bit is explicitly "for INAV XITL in Sitl mode (sends no
emulated sensor data)", i.e. the opposite of what you want here, per
`runtime_config.h:204`) does three things on real hardware:

1. Sets `ARMING_FLAG(SIMULATOR_MODE_HITL)`.
2. Substitutes whatever sensor data the packet carries — GPS fix/position
   (`gpsSolDRV`), attitude or raw accel/gyro, baro pressure, mag — for the
   FC's physical sensors, and feeds it through the *same* processing INAV
   would use for real sensors (`gpsProcessNewDriverData()` /
   `gpsProcessNewSolutionData()` for GPS, `runtime_config.h:189-204` for the
   full flag list controlling which fields are present).
3. Returns the FC's computed stabilized roll/pitch/yaw/throttle
   (`INPUT_STABILIZED_*`) in the response — the actual output of INAV's PID
   loop against whatever you fed it. This is what makes it a genuine closed
   loop rather than "fake GPS": if you integrate those outputs into your own
   simple physics and feed the result back as the next position, INAV's real
   nav code is now flying a simulated aircraft.

FF's own MSP connection (`MSPManager`) never speaks this command — you're
adding a second, independent MSP client (or driver process) that does.

## This changes how FF sees its own position — check which GNSS provider your build uses

This matters specifically for FF, and isn't something you'd hit testing
HITL against bare INAV. `GNSSManager` picks its own location from whichever
registered provider has a fix first (`GNSSManager::getLocation()`,
`GNSSManager.cpp:20-39`), and `main.cpp:139-150` registers providers
conditionally on build flags:

```cpp
#ifndef GNSS_INJECT
    gnssManager->addProvider(new MSP_GNSS());      // read position FROM the FC over MSP
#endif
#ifdef GNSS_ENABLED
    gnssManager->addProvider(new Direct_GNSS());   // a GPS chip wired directly to FF
#endif
#ifdef GNSS_INJECT
    gnssManager->addListener(new MSP_GNSS());       // push FF's position TO a GPS-less FC
#endif
```

On the common configuration (no `GNSS_INJECT`), `MSP_GNSS` is FF's primary
provider — `MSP_GNSS::loop()` polls the same `MSP_RAW_GPS` that
`MSPManager::getLocation()` (`MSPManager.cpp:174`) reads
(`MSP_GNSS.cpp:9-24`). That's the same field HITL's `gpsSolDRV` injection
feeds. Practical consequence: **once the follower FC is in HITL with a
simulated GPS fix, FF's own `/gnssmanager/status` starts reporting that
simulated, moving position automatically — you do not need
`/gnssmanager/spoof` at all** in this mode, unlike the bench-testing doc
where it's mandatory. You still need `/peermanager/spoof` for the leader,
since nothing here simulates a second aircraft.

If your target is built with `GNSS_INJECT` (FF has its own physical GPS chip
and is the position *source* for a GPS-less FC, e.g. an F411 target), this
inverts — check your board's `targets/*.ini` `build_flags` before assuming
either direction.

## Safety: HITL does not disarm your motors

Checked directly in INAV's source: `SIMULATOR_MODE_HITL` gates sensor
sourcing, OSD/beeper/battery-sim cosmetics, and one servo-autotrim skip —
nothing in `mixer.c` or `pwm_output.c` checks it. **Motor and servo PWM
output is not suppressed on real hardware in HITL mode.** If you arm with
props on and the simulated position is far enough from the target that
POSHOLD computes a correction, that correction reaches real motors. This is
a stricter requirement than the bench-testing doc's "props off" — there,
nothing airborne-shaped is being asked of the FC; here, you are deliberately
trying to get the nav loop to compute movement. **Props off, full stop,**
for everything in this doc.

## Two ways to actually drive MSP_SIMULATOR

Nothing about the protocol favors either path — pick based on how much
realism you need versus how much you want to install.

### Option A: INAV's own X-Plane HITL plugin (official, heavyweight)

INAV documents this directly:
[`docs/development/Hardware In The Loop (HITL) plugin for X-Plane.md`](https://github.com/iNavFlight/inav/blob/master/docs/development/Hardware%20In%20The%20Loop%20(HITL)%20plugin%20for%20X-Plane.md)
points at [INAV-X-Plane-HITL](https://github.com/RomanLut/INAV-X-Plane-HITL)
(or [INAV-X-Plane-XITL](https://github.com/Scavanger/INAV-X-Plane-XITL) for
INAV ≥ 9.0's newer HITL features). It's an X-Plane 11/12 plugin that
connects to the FC over USB, feeds it gyro/accel/baro/mag/GPS/pitot from
X-Plane's physics, and drives the aircraft's control surfaces/motors in
X-Plane from the FC's real output — full aerodynamic simulation, genuinely
closed-loop, "just works" once installed. The cost is X-Plane itself (paid,
non-trivial to set up) and general-purpose fixed-wing/multirotor physics
that has nothing to do with FF specifically. Reasonable if you already have
or are willing to get X-Plane; follow that project's own setup docs — it's
a community project outside both INAV's and FF's codebases, so specifics
(port selection, plugin version compatibility with `SIMULATOR_MSP_VERSION_2`
vs `_3`) can change independently of either.

### Option B: A minimal purpose-built driver (DIY, lightweight)

Since the wire format is fully visible in INAV's own source (cited above —
nothing obfuscated or SITL-only about it), a small script that speaks just
enough of `MSP_SIMULATOR` to exercise `FollowManager`'s capture behavior is
a realistic scope, and avoids installing a flight simulator just to check
"does the waypoint get captured." At minimum such a driver needs to:

- Send `MSP_SIMULATOR` (`0x201F`) with `flags = HITL_ENABLE |
  HITL_HAS_NEW_GPS_DATA` (add `HITL_USE_IMU` if you'd rather hand it raw
  accel/gyro and let INAV's own AHRS compute attitude, instead of sending
  attitude directly — see the flag list at `runtime_config.h:189-204`).
- Provide a plausible fixed attitude (level, `roll=pitch=yaw=0`) and 1G
  accel/gyro if you're not modeling rotation, a constant baro pressure for
  a chosen altitude, and a GPS fix (`fixType`, `numSat`, `lat`/`lon`/`alt`
  in the same 1e7/cm units MSP always uses, `groundSpeed`/`groundCourse`,
  `velNED`) that starts near wherever you're spoofing the leader.
- Read back the response's `INPUT_STABILIZED_ROLL/PITCH/YAW/THROTTLE` and,
  if you want genuine closed-loop capture behavior rather than just "GPS
  accepted," integrate a simple kinematic model from them each tick (lean
  angle → horizontal accel → velocity → position delta) to compute the next
  packet's position — this is the part that actually tests whether POSHOLD
  steers toward WP#255, since it's now responding to the FC's real control
  output instead of a scripted trajectory. Skipping this and just replaying
  a canned position sequence only proves the FC accepts moving GPS, not that
  its nav loop behaves correctly under it — worth knowing which one you're
  actually testing.

This is unofficial and not something FF or INAV ships — you're writing it.
Community prior art for the wire protocol itself (not the physics) exists
if you want a reference rather than starting from `fc_msp.c` cold:
[`msp_gps_toy`](https://codeberg.org/stronnag/msp_gps_toy) exercises
`MSP_SET_RAW_GPS`/`MSP2_SENSOR_GPS` injection specifically, and the SITL
Forge project's MSP-based HITL bridge (mentioned in its Steam listing) is
another real-hardware `MSP_SIMULATOR` driver in the same space — check
either project's own docs/license before reusing code, since both are
outside this codebase and can change independently of it.

## Wiring: simpler than SITL's TCP bridge, same principle as the bench doc

Because the FC here is real hardware with real UARTs, there's no
socat/TCP-port bridging to do — wire FF to one of the FC's UARTs exactly as
in the bench-testing doc. Run your HITL driver (X-Plane plugin or Option B
script) on a **different** UART or the FC's USB/VCP MSP connection, same
"FF and your tooling don't fight over one MSP port" principle
`sitl-testing-follow-mode.md` uses for bridging FF to a second SITL UART.
Enable MSP on whichever port each side uses via Configurator's Ports tab,
same as configuring any spare UART.

## Practical walkthrough

1. Flash the follower FC with normal INAV firmware for its target — no
   special HITL build. Wire FF to one UART, props off. Wire your HITL driver
   (X-Plane plugin, or Option B script) to a different UART or the FC's USB.
2. Start the HITL driver; send an initial `MSP_SIMULATOR` packet with
   `HITL_ENABLE` set and a starting GPS fix. Confirm `ARMING_FLAG(
   SIMULATOR_MODE_HITL)` took — Configurator's Setup tab GPS readout (or the
   OSD, if fed) should show the injected fix instead of whatever the FC's
   physical GPS reports.
3. `GET /gnssmanager/status` on FF — if your target isn't built with
   `GNSS_INJECT`, this should already be tracking the HITL-injected position
   without any `/gnssmanager/spoof` call (see above).
4. `POST /peermanager/spoof` with the leader positioned relative to FF's
   *current* (HITL-tracked) location — read that from step 3 first, since
   it's no longer a fixed value you chose, unlike the bench doc's fully
   static setup.
5. Arm the FC (props off!) and switch on `NAV POSHOLD` + `GCS NAV` from your
   transmitter, same as the bench doc — HITL doesn't change RC input at all,
   so if you already have a receiver wired for bench testing, keep using it.
6. Watch Configurator's WP list / Mission Control (pointed at the FC same as
   always) and, if you built a closed-loop Option B driver, watch the
   injected position in Configurator's GPS/map view over successive packets.
   With a closed-loop driver, the simulated position should visibly move
   toward and settle near WP#255 as `PeerLock` locks and the target updates
   — this is the thing bench spoofing structurally cannot show, and the gap
   the bench doc names explicitly.
7. Everything else from the bench doc's walkthrough (freshness guard,
   `GCS NAV` toggle resetting `PeerLock`, `HOLD_COURSE` stationary fallback,
   `targetSane()` refusing unsafe presets) still applies unchanged — HITL
   only replaces how the follower FC's position comes to exist, not
   anything about `FollowManager`'s own logic or how you exercise it.

## What this still doesn't buy you

- Real GPS noise, multipath, or vertical error — a simulated fix is exactly
  as clean as your driver makes it, same caveat as the bench doc's
  `FOLLOW_MIN_VSEP_M` discussion.
- Real RF link loss for peer telemetry — `/peermanager/spoof` is still an
  HTTP call on FF's own WiFi AP, not LoRa/ESP-NOW under real conditions.
- Real aerodynamics, wind, or prop wash. Option A (X-Plane) approximates
  these; a hand-rolled Option B kinematic integrator generally won't even
  try, and shouldn't be trusted as a stand-in for either.
- A HITL pass only proves the FC's nav code handled the trajectory your
  driver actually produced — it's not a substitute for the spec's
  progressive real-flight checklist (§12.2), just a much closer approximation
  than static bench spoofing before you get there.

Sources:
- [inav/src/main/target/common.h](https://github.com/iNavFlight/inav/blob/master/src/main/target/common.h)
- [inav/src/main/fc/fc_msp.c](https://github.com/iNavFlight/inav/blob/master/src/main/fc/fc_msp.c)
- [inav/src/main/fc/runtime_config.h](https://github.com/iNavFlight/inav/blob/master/src/main/fc/runtime_config.h)
- [inav/src/main/msp/msp_protocol_v2_inav.h](https://github.com/iNavFlight/inav/blob/master/src/main/msp/msp_protocol_v2_inav.h)
- [inav/docs/development/Hardware In The Loop (HITL) plugin for X-Plane.md](https://github.com/iNavFlight/inav/blob/master/docs/development/Hardware%20In%20The%20Loop%20(HITL)%20plugin%20for%20X-Plane.md)
- [RomanLut/INAV-X-Plane-HITL](https://github.com/RomanLut/INAV-X-Plane-HITL)
- [Scavanger/INAV-X-Plane-XITL](https://github.com/Scavanger/INAV-X-Plane-XITL)
- [stronnag/msp_gps_toy](https://codeberg.org/stronnag/msp_gps_toy)
