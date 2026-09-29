# HITL: real flight controller, simulated movement

[`bench-testing-follow-mode.md`](bench-testing-follow-mode.md) manufactures the
*leader* with the firmware's built-in traffic simulator, which is enough to
exercise the geometry, the lock state machine and the MSP wiring. What it
cannot show is whether the follower's flight controller actually captures and
holds the commanded waypoint: that is INAV's POSHOLD and nav-to-waypoint loop,
not FF's. It also cannot give the follower a *moving* position of its own, since
v2 has no way to inject one.

[`sitl-testing-follow-mode.md`](sitl-testing-follow-mode.md) removes the
physical flight controller entirely by running INAV as a PC binary, but loses
the real flight controller in the process.

This doc covers the option in between: keep a *real* flight controller, real
silicon, real firmware, unmodified, but replace its sensor inputs (GPS, IMU,
baro, mag) with simulated values fed over MSP, so its own navigation code
believes it is flying and reacts accordingly. This is hardware-in-the-loop
(HITL). FF itself stays real hardware throughout, and the leader still comes
from FF's own traffic simulator.

> Nothing in v2 has been validated on hardware yet, this document included.

## What makes this possible: INAV ships HITL support in normal firmware

This is not a special build. `USE_SIMULATOR` is defined in
[`target/common.h`](https://github.com/iNavFlight/inav/blob/master/src/main/target/common.h),
which is included by essentially every INAV hardware target, so the same
firmware you would normally flash to a real FC already contains this code path.
(If you want to be certain for your board, check that your target's
`.h`/`CMakeLists.txt` does not `#undef` it. This is rare.)

The mechanism is the MSP2 command `MSP_SIMULATOR` (`0x201F`, defined in
`msp_protocol_v2_inav.h`), handled by `mspProcessSimulatorCommand()` and
`readMspSimulatorValues()` in
[`fc_msp.c`](https://github.com/iNavFlight/inav/blob/master/src/main/fc/fc_msp.c).
Sending it with the `HITL_ENABLE` flag set, and `HITL_SITL_MODE` left *clear*
(that bit means "INAV XITL in SITL mode, sends no emulated sensor data", the
opposite of what you want here), does three things on real hardware:

1. Sets `ARMING_FLAG(SIMULATOR_MODE_HITL)`.
2. Substitutes whatever sensor data the packet carries, GPS fix and position
   (`gpsSolDRV`), attitude or raw accel/gyro, baro pressure, mag, for the FC's
   physical sensors, and feeds it through the *same* processing INAV would use
   for real sensors.
3. Returns the FC's computed stabilized roll/pitch/yaw/throttle
   (`INPUT_STABILIZED_*`) in the response, which is the actual output of INAV's
   PID loop against whatever you fed it. This is what makes it a genuine closed
   loop rather than "fake GPS": integrate those outputs into your own simple
   physics and feed the result back as the next position, and INAV's real nav
   code is flying a simulated aircraft.

FF's own MSP link (`hal/MspFcLink`) never speaks this command. You are adding a
second, independent MSP client that does.

## HITL gives FF a moving position of its own, for free

This is the part that matters for FF specifically, and it is why HITL is worth
the setup over plain bench testing.

On a normal target, FF's own location comes from the flight controller:
`MspFcLink` is the `ILocationSource`, and it polls `MSP_RAW_GPS` on its own
schedule. That is the same field HITL's `gpsSolDRV` injection feeds. So once
the follower FC is in HITL with a simulated GPS fix, `/api/status` on FF starts
reporting that simulated, moving position automatically, with nothing to
configure. Bench testing has no equivalent: v2 has no position-injection
endpoint, so the only other way to get a valid own-position is a real GPS fix.

The exception is a target built with `GNSS_ENABLED`, where a GNSS receiver
wired directly to the FF board is the location source instead, and the FC's
injected fix is ignored for this purpose. Check your board's `targets/*.ini`
`build_flags` before assuming either direction.

The leader still comes from FF's traffic simulator (`POST /api/sim/peer`),
since nothing in HITL simulates a second aircraft. Place it relative to the
follower's *current* HITL-tracked position, which you read from `/api/status`
rather than choosing up front.

## Safety: HITL does not disarm your motors

`SIMULATOR_MODE_HITL` gates sensor sourcing, some OSD/beeper/battery-sim
cosmetics and one servo-autotrim skip. Nothing in `mixer.c` or `pwm_output.c`
checks it. *Motor and servo PWM output is not suppressed on real hardware in
HITL mode.* If you arm with props on and the simulated position is far enough
from the target that POSHOLD computes a correction, that correction reaches
real motors. This is stricter than the bench doc's "props off": there, nothing
airborne-shaped is being asked of the FC; here you are deliberately trying to
get the nav loop to compute movement. *Props off, full stop,* for everything in
this doc.

## Two ways to drive MSP_SIMULATOR

Nothing about the protocol favors either path. Pick based on how much realism
you need against how much you want to install.

### Option A: INAV's own X-Plane HITL plugin (official, heavyweight)

INAV documents this directly:
[`Hardware In The Loop (HITL) plugin for X-Plane.md`](https://github.com/iNavFlight/inav/blob/master/docs/development/Hardware%20In%20The%20Loop%20(HITL)%20plugin%20for%20X-Plane.md)
points at [INAV-X-Plane-HITL](https://github.com/RomanLut/INAV-X-Plane-HITL),
or [INAV-X-Plane-XITL](https://github.com/Scavanger/INAV-X-Plane-XITL) for
INAV 9.0 and later. It is an X-Plane 11/12 plugin that connects to the FC over
USB, feeds it gyro, accel, baro, mag, GPS and pitot from X-Plane's physics, and
drives the aircraft's control surfaces and motors in X-Plane from the FC's real
output. Full aerodynamic simulation, genuinely closed loop, and it works once
installed. The cost is X-Plane itself (paid, non-trivial to set up) and
general-purpose physics that has nothing to do with FF specifically. Follow that
project's own setup docs: it is a community project outside both INAV's and
FF's codebases, so port selection and plugin/protocol version compatibility can
change independently of either.

### Option B: a minimal purpose-built driver (DIY, lightweight)

The wire format is fully visible in INAV's own source, so a small script that
speaks just enough of `MSP_SIMULATOR` to exercise capture behavior is a
realistic scope, and avoids installing a flight simulator to answer "does the
waypoint get captured". At minimum such a driver needs to:

- Send `MSP_SIMULATOR` (`0x201F`) with
  `flags = HITL_ENABLE | HITL_HAS_NEW_GPS_DATA` (add `HITL_USE_IMU` if you
  would rather hand it raw accel and gyro and let INAV's AHRS compute attitude
  instead of sending attitude directly).
- Provide a plausible fixed attitude (level, roll = pitch = yaw = 0) and 1 G
  accel/gyro if you are not modeling rotation, a constant baro pressure for a
  chosen altitude, and a GPS fix (`fixType`, `numSat`, lat/lon/alt in the same
  1e7 and cm units MSP always uses, `groundSpeed`, `groundCourse`, `velNED`)
  that starts near where you are placing the simulated leader.
- Read back the response's `INPUT_STABILIZED_ROLL/PITCH/YAW/THROTTLE` and, for
  genuine closed-loop capture behavior rather than just "GPS accepted",
  integrate a simple kinematic model from them each tick (lean angle to
  horizontal accel to velocity to position delta) to compute the next packet's
  position. This is the part that actually tests whether POSHOLD steers toward
  waypoint 255, since it is now responding to the FC's real control output
  rather than a scripted trajectory. Skipping it and replaying a canned
  position sequence only proves the FC accepts moving GPS. Worth knowing which
  of the two you are testing.

This is unofficial and not something FF or INAV ships: you are writing it.
Community prior art for the wire protocol (not the physics) exists if you want
a reference rather than starting from `fc_msp.c` cold:
[`msp_gps_toy`](https://codeberg.org/stronnag/msp_gps_toy) exercises
`MSP_SET_RAW_GPS`/`MSP2_SENSOR_GPS` injection specifically. Check any such
project's own docs and license before reusing code.

## Wiring: simpler than SITL's TCP bridge

The FC here is real hardware with real UARTs, so there is no socat or TCP
bridging. Wire FF to one of the FC's UARTs exactly as in the bench-testing doc,
and run your HITL driver (X-Plane plugin or Option B script) on a *different*
UART or on the FC's USB/VCP MSP connection, so FF and your tooling do not fight
over one MSP port. Enable MSP on whichever port each side uses via
Configurator's Ports tab.

## Practical walkthrough

1. Flash the follower FC with normal INAV firmware for its target. No special
   HITL build. Wire FF to one UART, props off. Wire your HITL driver to a
   different UART or the FC's USB.
2. Start the HITL driver and send an initial `MSP_SIMULATOR` packet with
   `HITL_ENABLE` set and a starting GPS fix. Confirm it took:
   Configurator's Setup tab GPS readout, or the OSD, should show the injected
   fix rather than whatever the FC's physical GPS reports.
3. `GET /api/status` on FF. Unless your target is built with `GNSS_ENABLED`,
   `location.valid` should be true and `location.lat`/`lon` should be tracking
   the injected position, with no further configuration.
4. Enable FF's simulator and add a leader positioned relative to that current
   position:

   ```bash
   curl -X POST -H 'Content-Type: application/json' \
        -d '{"sim": {"enabled": true}}' http://192.168.4.1/api/config
   curl -X POST -H 'Content-Type: application/json' \
        -d '{"name":"SIM1","mode":"circle","lat":51.5001,"lon":-0.1,
             "alt_m":120,"speed_ms":12,"radius_m":120}' \
        http://192.168.4.1/api/sim/peer
   ```

   Keep the leader within `maxTargetDistM` (50 m by default) of the follower,
   or the solved target is refused and the condition GVAR reads `2`.
5. Arm the FC (props off) and switch on `NAV POSHOLD` plus `GCS NAV` from your
   transmitter. HITL does not change RC input at all, so an existing bench
   receiver keeps working.
6. Watch two things at once: FF's *Follow* page (lock state, the target it is
   solving, its age) and, if you built a closed-loop Option B driver, the
   injected position in Configurator's GPS or map view over successive packets.
   With a closed-loop driver the simulated position should visibly move toward
   and settle near waypoint 255 as the lock holds and the target updates. This
   is the thing bench testing structurally cannot show.
7. Everything else from the bench walkthrough still applies unchanged: the
   freshness guard (delete the simulated peer and watch `LOCKED_HOLDING`), the
   `GCS NAV` toggle returning the state to `IDLE`, the stationary-course
   fallback below `minCourseSpeed`, and the config validator refusing an unsafe
   slot. HITL only changes how the follower's own position comes to exist.

## What this still does not buy you

- Real GPS noise, multipath or vertical error. A simulated fix is exactly as
  clean as your driver makes it, the same caveat as the bench doc's `minVSepM`
  discussion.
- Real RF link loss for peer telemetry. The leader still arrives on FF's
  virtual radio, which never drops a frame, never collides and has no range.
- Real aerodynamics, wind or prop wash. Option A approximates these; a
  hand-rolled Option B kinematic integrator generally will not even try, and
  should not be trusted as a stand-in.
- A HITL pass only proves the FC's nav code handled the trajectory your driver
  actually produced. It is not a substitute for a progressive real-flight
  checklist, just a much closer approximation than a static bench.

Sources:
- [inav/src/main/target/common.h](https://github.com/iNavFlight/inav/blob/master/src/main/target/common.h)
- [inav/src/main/fc/fc_msp.c](https://github.com/iNavFlight/inav/blob/master/src/main/fc/fc_msp.c)
- [inav/src/main/fc/runtime_config.h](https://github.com/iNavFlight/inav/blob/master/src/main/fc/runtime_config.h)
- [inav/src/main/msp/msp_protocol_v2_inav.h](https://github.com/iNavFlight/inav/blob/master/src/main/msp/msp_protocol_v2_inav.h)
- [inav/docs/development/Hardware In The Loop (HITL) plugin for X-Plane.md](https://github.com/iNavFlight/inav/blob/master/docs/development/Hardware%20In%20The%20Loop%20(HITL)%20plugin%20for%20X-Plane.md)
- [RomanLut/INAV-X-Plane-HITL](https://github.com/RomanLut/INAV-X-Plane-HITL)
- [Scavanger/INAV-X-Plane-XITL](https://github.com/Scavanger/INAV-X-Plane-XITL)
- [stronnag/msp_gps_toy](https://codeberg.org/stronnag/msp_gps_toy)
