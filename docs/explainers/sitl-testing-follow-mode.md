# Testing Follow Mode with INAV SITL

[`bench-testing-follow-mode.md`](bench-testing-follow-mode.md) covers testing
`FollowManager` with a real flight controller on the bench, props off, and
punts on one piece: the altitude/`GCS NAV` gate needs *something* on the
other end of FF's MSP connection that speaks real INAV MSP, and a real FC is
one option. This doc covers the other option — INAV SITL — which gets you to
zero physical aircraft hardware at all on the follower side. FF (the ESP32)
is still real hardware; only the flight controller is simulated.

## What INAV SITL actually is

SITL ("Software In The Loop") is INAV's own firmware source compiled as a
normal PC binary instead of cross-compiled for an STM32 flight controller.
It runs the same MSP command handling, mode-bit logic, and (if you feed it
sensor data) navigation code as real INAV — FF's MSP connection genuinely
cannot tell it apart from a physical FC. The parts that differ are the parts
FF's MSP link never touches: physical sensor drivers, motor PWM output, and
the serial hardware itself.

Two things follow from that:

- **You don't need a flight simulator (X-Plane/RealFlight) hooked up.**
  Started with no arguments, SITL runs in what its own launch script calls
  "configurator-only mode" — it exposes MSP, tracks arm state and mode bits
  from RC input, and reports whatever sensor state it has (a fixed/static
  attitude and altitude with nothing driving them) without any external
  flight dynamics feed. That's sufficient for what this codebase's bench
  checklist needs from the FC side: `MSP_ALTITUDE`, `MSP_STATUS`/`MSP_BOXIDS`
  mode bits (arm, `GCS NAV`), and accepting `MSP_SET_WP` for waypoint 255.
  Hooking up X-Plane only matters if you want the aircraft to actually *fly*
  to the follow target, which is out of scope for validating FF's math and
  state machine.
- **MSP no longer arrives over a UART — it arrives over TCP.** SITL replaces
  each of INAV's UARTs with a TCP listener: UART1 → port 5760, UART2 → 5761,
  and so on, ascending. FF, however, is real ESP32 firmware that only knows
  how to speak MSP over a physical hardware UART (`Serial1`, wired to
  whichever pins your target's `.ini` sets — e.g. `SERIAL_PIN_RX=17` /
  `SERIAL_PIN_TX=23` for the LilyGO T-Beam entries in `targets/legacy.ini`).
  Bridging that gap is the one genuinely fiddly part of this setup, covered
  below.

## Setting up SITL

The easiest path is INAV's Docker build, since it sidesteps installing a
cross-toolchain/cmake/ninja locally just to get a SITL binary:

```bash
git clone https://github.com/iNavFlight/inav.git
cd inav
# builds an inav-build Docker image per docs/development/Building in Docker.md,
# then runs cmake -DSITL=ON -GNinja + ninja inside it into build_SITL/
docker run --rm --entrypoint /src/cmake/docker_build_sitl.sh -it -v "$PWD":/src inav-build
```

That produces `build_SITL/inav_<version>_SITL` in your checkout. Run it
directly (no Docker needed at runtime unless you prefer it):

```bash
cd build_SITL
./inav_7.0.0_SITL          # configurator-only mode: no external sim, just MSP + mode logic
```

If you'd rather build natively (no Docker), the underlying commands are the
same ones the Docker script runs — `cmake -DSITL=ON -GNinja -B build_SITL ..`
then `ninja` from a `build_SITL` directory — see INAV's own
[`docs/development/Building in Linux.md`](https://github.com/iNavFlight/inav/blob/master/docs/development/Building%20in%20Linux.md)
(or the Windows/WSL or macOS equivalents in the same folder) for prerequisites,
since that's an INAV build-environment concern, not an FF one.

Connect INAV Configurator to it by selecting the "SITL" entry in the
connection dropdown, or manually as TCP to `localhost:5760`. Use Configurator
normally from here to set up `NAV POSHOLD` + `GCS NAV` on an AUX channel
(Modes tab) exactly as you would with real hardware — nothing about SITL
changes INAV's own configuration workflow.

### Arming and switching modes without a transmitter

SITL still needs *some* RC input to drive arm state and AUX mode ranges.
INAV supports RX_MSP (18 channels injected over the same MSP TCP
connection) as SITL's default, and also accepts an actual RC transmitter in
USB-joystick mode (select "SIM (SITL)" as the joystick target in
Configurator) if you have a transmitter that supports that — this is
usually the path of least resistance if you already own one, since it's
real stick/switch input instead of scripting channel injection.

## Bridging FF's UART to SITL's TCP port

This is the part that's specific to FF and not covered by INAV's own docs,
since FF is real hardware sitting outside SITL entirely. You need something
that presents a serial port on one side (for FF's ESP32 `Serial1` pins to
talk to) and speaks TCP on the other (to reach SITL's port). A USB-to-serial
adapter (FTDI/CP2102) plus `socat` is the simplest combination on
Linux/macOS:

1. Wire the USB-serial adapter's TX/RX/GND to the ESP32's `Serial1` pins,
   **crossed** (adapter TX → ESP32 RX pin, adapter RX → ESP32 TX pin) —
   same as wiring any FC-to-peripheral UART.
2. Leave UART1 (port 5760) for Configurator, and bridge a second SITL UART —
   say UART2 (port 5761) — to the adapter, so FF and Configurator can be
   connected simultaneously without fighting over the same MSP port, mirroring
   how a real aircraft has FF and Configurator on separate physical UARTs:

   ```bash
   socat -d -d /dev/ttyUSB0,raw,echo=0,b115200 TCP:127.0.0.1:5761
   ```

   Match the baud rate to FF's `SERIAL_SPEED` (`src/main.h:61`, 115200
   currently) — the TCP side isn't actually rate-limited, but the physical
   adapter↔ESP32 link is, so mismatching it will corrupt the MSP stream on
   that leg.
3. In Configurator (connected to 5760, i.e. a different UART than FF is
   bridged to), enable MSP on that second UART's port config the same way
   you'd enable it on any spare real UART.

On Windows, the equivalent is a virtual COM port pair (e.g. `com0com`) with
a small serial↔TCP relay bridging one end to SITL's port — INAV's
Configurator resources bundle a tool for exactly this per the SITL docs;
check there since the exact tool/version is an INAV/Configurator concern
that can change independently of FF.

Flash FF onto the ESP32 as normal, wired to the adapter as above instead of
a real FC, and everything from the bench-testing doc's fake-a-leader /
fake-your-own-GPS steps applies unchanged — `/peermanager/spoof` and
`/gnssmanager/spoof` don't care whether the FC on the other end of MSP is
silicon or a PC process.

## What this buys you over the bench-with-real-FC approach

- No physical aircraft, no props to worry about removing, no FC hardware
  needed at all beyond the ESP32 running FF itself.
- Config changes (arming, AUX ranges, `GCS NAV` assignment) are Configurator
  clicks against a process you can restart in seconds if you get the mode
  setup wrong, rather than re-flashing or power-cycling real hardware.
- `GET /followmanager/status` (added in the Phase 2 read-only status
  endpoint) plus Configurator's live Mission Control / WP list tab pointed
  at SITL's own port together give you the same visibility the bench doc
  describes, without needing an OSD or a second physical unit.

## What it still doesn't buy you

Same caveat as the bench doc, just restated for SITL specifically: nothing
here exercises real GPS noise, real RF link margin for the peer telemetry,
or whether the follower FC's nav loop actually *flies* the waypoint well in
the air — SITL without a flight-dynamics sim attached never leaves the
ground, and SITL's simulated GPS (when a sim *is* attached) is still not
real-world GPS error. Use this to validate FF's geometry math, the
peer-lock state machine, and the MSP wiring quickly and repeatably; keep the
spec's progressive real-flight checklist (§12.2) as the final gate before
trusting the safety margins for real.

Sources:
- [inav/docs/SITL/SITL.md](https://github.com/iNavFlight/inav/blob/master/docs/SITL/SITL.md)
- [inav/docs/development/Building in Docker.md](https://github.com/iNavFlight/inav/blob/master/docs/development/Building%20in%20Docker.md)
- [inav/cmake/docker_build_sitl.sh](https://github.com/iNavFlight/inav/blob/master/cmake/docker_build_sitl.sh)
- [inav/cmake/docker_run_sitl.sh](https://github.com/iNavFlight/inav/blob/master/cmake/docker_run_sitl.sh)
