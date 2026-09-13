# Bench-testing Follow Mode without flying

`ff::FollowController` (`lib/ff_core/follow.cpp`) computes a waypoint from a
leader's telemetry and pushes it to the follower's flight controller. The
leader can be manufactured by the firmware itself, so validating the geometry
math and the peer-lock state machine does not need a second aircraft, or an
aircraft in the air at all.

What it does still need is a position fix on the node itself and a flight
controller on the other end of MSP. Both are covered below.

> This is the v2 firmware. The v1 `/peermanager/spoof` and `/gnssmanager/spoof`
> endpoints are gone, along with everything else under those managers. Nothing
> in v2 has been validated on hardware yet, this document included.

## Why this is safe to do on the bench

Follow only ever calls `IFollowFc::sendFollowWaypoint()`, implemented by
`hal/MspFcLink`, which sends a single `MSP_SET_WP` (#209) for waypoint 255, the
same slot INAV's own follow-me feature reads. It does not touch motors, arm the
aircraft, or command flight directly. Whether the aircraft *acts* on that
waypoint depends entirely on the flight controller's arm state and active
flight modes (`NAV POSHOLD` plus `GCS NAV`). So the standard rule applies:
props off, and do not arm with props on while doing any of this.

## The three things Follow needs, and where each comes from on a bench

| Input | Real source | On the bench |
|---|---|---|
| Leader position, course and speed | A second aircraft's beacons over LoRa or ESP-NOW | The built-in traffic simulator, `POST /api/sim/peer` |
| This node's own position | The FC over MSP, or a directly attached GNSS receiver | The same. There is no position-injection endpoint in v2. See §2 |
| This node's altitude and the `GCS NAV` mode bit | The flight controller (`MSP_ALTITUDE`, `MSP2_INAV_STATUS`) | A real INAV FC with props off, or INAV SITL. Cannot be faked from FF's API |

The node's AP is `FormationFlight-<uid>`, open unless you set a password, and
the web UI and API are at `192.168.4.1`.

## 1. Manufacturing a leader

The simulator produces peer motion in closed form (`lib/ff_core/sim_traffic`),
encodes each position into a real protocol packet, encrypts it with the real
group key as though it came from that peer, and pushes it through
`Node::onReceive` from a virtual radio (`src/hal/SimRadio`). The peer table,
Follow and the MSP radar output cannot tell it from RF, because as far as the
receive path is concerned it *is* RF: same decode, same AES-CCM
authentication, same replay check. Simulating further up the stack would test
the simulator instead of the firmware.

Switch it on first. It is refused while off, deliberately:

```bash
curl -X POST -H 'Content-Type: application/json' \
     -d '{"sim": {"enabled": true}}' http://192.168.4.1/api/config
curl -X POST http://192.168.4.1/api/config/save
```

Then add a peer:

```bash
# A leader flying a closed hexagon, 150 m sides, 15 m/s, centred on a
# position near your own, climbing and descending as it goes round
curl -X POST -H 'Content-Type: application/json' \
     -d '{"name": "SIM1", "mode": "hex", "lat": 51.5, "lon": -0.1,
          "alt_m": 120, "speed_ms": 15, "radius_m": 150}' \
     http://192.168.4.1/api/sim/peer

# What is running
curl -s http://192.168.4.1/api/sim
```

Everything above is also on the *Simulator* page of the web UI, which pre-fills
the latitude and longitude from the node's own fix so you are not typing
coordinates by hand.

`mode` is one of:

| Mode | Path |
|---|---|
| `static` | Sits at `lat`/`lon`/`alt_m`, reporting `course_deg` and zero speed |
| `line` | Departs `lat`/`lon` on `course_deg` at `speed_ms` and never turns |
| `circle` | Orbits `lat`/`lon` at `radius_m` and `speed_ms` |
| `hex` | Closed hexagon of side `radius_m` centred on `lat`/`lon`, climbing linearly to a peak 80 m above the start altitude at the half-way vertex and descending back by the time the loop closes |

`hex` is the one worth using for a tracking test. The course changes in 60 deg
steps at each vertex rather than smoothly, the altitude ramps up and back down
across the whole loop rather than per edge, and the loop closes, so the
follower has to cope with all three. The climb height is
`ff::kSimHexClimbM`, 80 m.

Notes and limits:

- `lat` and `lon` here are decimal degrees, not the 1e7 integers the wire
  protocol uses, because a human types them into a form.
- `POST /api/sim/peer` replaces a peer rather than merging into it, and restarts
  its path from the moment of the call. Send the whole peer every time: any key
  you leave out takes its default.
- `speed_ms` is 0 to 200 m/s. `radius_m` must be greater than 0 for `circle`
  and `hex`.
- `uid` is optional. Omit it and the device generates one in the `5eed0000`
  range, so a simulated node is recognisable as one even in a raw packet
  capture.
- Up to 4 simulated peers run at once (`SimRadio::kMaxSimPeers`).
- `DELETE /api/sim/peer?uid=...` removes one, `POST /api/sim/clear` removes all
  of them, and switching `sim.enabled` off clears them too.
- Every page of the UI carries a banner while the simulator is on, and
  `/api/status` marks the virtual radio with `"sim": true` in the `radios`
  array. Treat both as load-bearing: nothing downstream of the receive path has
  any way to know a peer is not real.
- `sim.enabled` is saved to flash the moment you toggle it in the UI, on
  purpose. A node that boots with simulated traffic running and nothing to say
  so is exactly the trap that setting guards against.

## 2. Your own position, which you cannot fake

Two parts of Follow's math depend on this node's own location, and it has no
override in v2:

- The `maxTargetDistM` sanity check measures from your own position to the
  solved target.
- The commanded altitude is `localAltitudeCm() + (peer.alt_m - self.alt_m) ×
  100 + vertical_m × 100`, so your own altitude sets the relative-altitude
  term.

Without a valid fix, Follow still locks onto a peer but emits no waypoint at
all. The *Follow* page's *Live* card says "This node has no position fix" in
its pre-arm warnings, and `/api/status`'s `location.valid` is false. If you are
chasing a "why is nothing being sent" problem, check this first: it looks
exactly like a gate bug and is not one.

Your options on a bench:

- A flight controller with a real GPS fix, which usually means putting the
  bench near a window or running an antenna outside.
- A GNSS receiver wired directly to the FF node (`GNSS_ENABLED` targets), same
  problem, same solution.
- HITL, where the GPS fix is injected into a real flight controller by a
  driver and FF reads it back over MSP like any other fix. This is the only way
  to get a *moving* own-position on a bench. See
  [`hitl-testing-follow-mode.md`](hitl-testing-follow-mode.md).

## 3. Altitude and the `GCS NAV` gate: a real or SITL FC

These genuinely cannot come from anywhere else:

- `MspFcLink` polls `MSP_ALTITUDE` (#109) for the FC's own baro/GPS-fused
  home-relative altitude estimate. There is no FF-side override.
- With the shipped default `FOLLOW_TRIGGER_MODE = FOLLOW_TRIGGER_GCSNAV`, the
  gate is INAV's `GCS NAV` box, read from `MSP2_INAV_STATUS`'s full-width box
  bitmask. That bit only exists on the FC. (`FOLLOW_TRIGGER_AUX` is a
  placeholder: `followSwitchActive()` returns false for it, deliberately,
  rather than silently defaulting on.)

So you need one of:

- *A real INAV flight controller, props removed*, wired to FF over its normal
  MSP UART. Arm it (props off) and switch on `NAV POSHOLD` plus `GCS NAV` from
  your transmitter, the same as testing any other nav mode on the bench.
- *INAV SITL*, which removes the physical flight controller entirely. See
  [`sitl-testing-follow-mode.md`](sitl-testing-follow-mode.md) for the setup
  and for bridging FF's UART to SITL's TCP port.

Either way, watch which mode bits are actually active rather than assuming your
switch did what you think. The *Live* card's *Trigger gate* row reports exactly
what FF believes, and Configurator's Status tab reports what INAV believes.

## Watching what Follow is doing

Unlike v1, you do not have to infer any of this from the FC side:

- `GET /api/status` has a `follow` object with the lock state, the gate, the
  locked UID and name, the last target (position, altitude, heading, age), the
  live offset after RC trim, the pre-arm candidate offset, and the GVAR values
  last written. Fields that have never been computed are absent rather than
  zero.
- The *Follow* page renders the same thing, with a pre-arm warning list for the
  handful of conditions that will stop Follow doing anything useful the moment
  the trigger goes active.
- `GET /api/frames` is the 32-entry frame log, newest first, with a `result`
  per frame (`tx`, `ok`, `self`, `crypto_fail`, `replay_fail`, `decode_fail`,
  `oversize`). This is where you look when frames are arriving but peers are
  not appearing.
- Configurator's Mission Control or waypoint list, pointed at the same FC,
  shows waypoint 255 updating at `emitHz` while the gate is active. Read back
  `MSP_WP` (#254) directly if you are scripting against the FC yourself.

## Cross-checking the geometry by hand

`ff::slotToLatLon()` and the `ff::geo` module are pure math with no hardware
dependency, so you can compute the expected target independently and compare it
against what the API reports. This is how you catch a scaling or sign error
rather than eyeballing "it looks about right on the map".

The projection, in the leader's track-relative frame:

```
north_m = long_m·cos(course) − lat_m·sin(course)
east_m  = long_m·sin(course) + lat_m·cos(course)
distance = √(north_m² + east_m²)
bearing  = atan2(east_m, north_m)   (normalised to 0-360°)
```

then a standard great-circle projection from the leader's lat/lon at that
distance and bearing (`ff::geo::pointAtDistance`).

Worked example, the chase-high default (`ofsLongM = -15`, `ofsLatM = 0`,
`ofsVertM = 10`) with a leader at 51.500000, -0.100000 on a course of 0 deg:

- `north_m = -15·cos(0) − 0·sin(0) = -15 m`, `east_m = -15·sin(0) + 0·cos(0) = 0 m`
- `distance = 15 m`, `bearing = atan2(0, -15) = 180 deg`, due south. Behind a
  north-bound leader is south of it, as expected.
- Projecting 15 m south from 51.500000, -0.100000 lands at approximately
  51.499865, -0.100000. The latitude delta is about 15 / 111320 = 0.000135 deg,
  and longitude is unchanged at a due-south bearing.

For altitude, set the simulated peer's `alt_m` relative to your own reported
altitude (`location.alt_m` in `/api/status`) so the relative-altitude term
comes out to something realistic, then confirm the vertical offset is added
with the right sign against whatever the FC reports for
`localAltitudeCm()`. Chasing an absolute number is less useful than confirming
the sign and the magnitude of the change.

The whole of this math is also covered by host tests
(`test/test_geo`, `test/test_follow`, `test/test_sim_traffic`), which run
without any hardware at all:

```bash
pio test -e native
```

## Practical bench walkthrough

1. Flash FF onto your bench node. Connect it to a real or SITL INAV FC over
   MSP, props removed. Join the node's WiFi AP.
2. Confirm `/api/status` reports `location.valid: true`. If it does not, stop
   and fix that first (§2): nothing downstream will work and every symptom will
   mislead you.
3. Enable the simulator and add a peer (§1). Start with `mode: "static"`
   placed a few tens of meters from your own position, so the geometry is easy
   to check by hand.
4. Confirm the peer appears in `/api/status`'s `peers` array with the right
   position, course and speed, and a small `age_ms`.
5. Arm the FC (props off) and switch on `NAV POSHOLD` plus `GCS NAV`. The
   *Live* card should go `IDLE` to `ACQUIRING` to `LOCKED`.
6. Compare `follow.target` against your hand-computed target from the previous
   section. Watch waypoint 255 update in Configurator at `emitHz`.
7. Delete the simulated peer (`DELETE /api/sim/peer?uid=...`) and confirm the
   state goes to `LOCKED_HOLDING` within `peerTimeoutMs` (1500 ms by default)
   and no further waypoints go out. That is the freshness guard.
8. Toggle `GCS NAV` off and confirm emission stops immediately and the state
   returns to `IDLE`.
9. Re-add the peer in `line` mode below `minCourseSpeed` (2 m/s by default) and
   confirm the slot's orientation holds the last trusted course rather than
   following a near-zero course reading. Bracket the threshold deliberately,
   for example `speed_ms: 1.5` and `speed_ms: 3`.
10. Post a slot that violates `minSepM` or `minVSepM` and confirm the config is
    rejected with a message, rather than accepted and then quietly unsafe.
11. Switch to `hex` and watch a full loop: stepped course changes at each
    vertex, the altitude ramp up and back, and the slot rotating with the
    leader through all six legs.

None of this needs props on, a second aircraft, or anything in the air.

## Developing with no hardware at all

`scripts/mock_server.py` implements every endpoint in
[`../v2-web-api.md`](../v2-web-api.md) against an in-memory node whose peers
actually fly, whose counters climb, and whose Follow state walks
`IDLE` to `ACQUIRING` to `LOCKED`. It serves `html/` directly, so the web UI
can be developed and exercised in a browser with nothing plugged in:

```bash
python3 scripts/mock_server.py --sim
```

It is a mock, not the firmware: use it for UI work and for checking what a
client sees, not for validating firmware behavior. `scripts/run_tests.sh` runs
the native suites, the UI's `follow-logic.js` tests and the mock server's own
tests together.

## What still needs a real flight

Bench testing validates the math, the state machine and the MSP wiring. It does
not validate:

- Real GPS accuracy and noise on both aircraft, which is exactly what
  `minVSepM`'s 13 m default (roughly 5 m of physical clearance plus 8 m of
  assumed GPS vertical error) is trying to absorb. A two-real-GPS check at a
  known height difference is worth doing before trusting that margin.
- Whether the follower's flight controller actually captures and holds the
  commanded waypoint in the air. That is INAV's POSHOLD and nav-to-waypoint
  loop, not FF's, and a simulated peer cannot exercise it. HITL gets closer.
- LoRa and ESP-NOW link margin, and peer staleness under real RF. A simulated
  peer arrives on a virtual radio that never drops a frame, never collides and
  has no range.

Keep bench testing as the fast, safe, repeatable first pass, and treat the
progressive flight checklist (trail slot, generous gaps, low speed, manual
override tested first) as the real gate.
