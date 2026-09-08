# Bench-Testing Follow Mode Without Flying

`FollowManager` (`src/lib/Follow/FollowManager.cpp`) computes a waypoint from a
leader's telemetry and pushes it to the follower's flight controller. Every
input it needs — the leader's GPS, the follower's own GPS, and the follower's
altitude — can be faked over HTTP or read from a flight controller sitting on
a bench with its props off. Nothing about validating the *geometry math* or
the *peer-lock state machine* requires a real GPS fix or an airborne aircraft.
This is also what the implementation plan
(`docs/plans/2026-07-31-FollowMeOnInav-Plan.md`) and spec
(`docs/spec/2026-07-31-FollowMeOnInav.md` §12.1) assume — this doc is the
practical walkthrough of running that checklist.

## Why this is safe to do with props on the bench

`FollowManager::loop()` only ever calls
`MSPManager::sendFollowWaypoint()` (`MSPManager.cpp:270`), which sends a
single `MSP_SET_WP` (#209) for waypoint **255** — the same slot INAV's
"follow me" feature already reads. It does not touch motors, arm the
aircraft, or otherwise command flight directly. Whether the aircraft *acts*
on that waypoint depends entirely on the flight controller's own arm state
and active flight modes (`NAV POSHOLD` + `GCS NAV`, spec §4). So the standard
rule applies: **props off, and don't arm with props on** while doing any of
this. Everything below can be exercised with the FC connected over USB with
propellers removed.

## The three things FollowManager needs, and how to fake each one

| Input | Real source | How to fake it |
|---|---|---|
| Leader position/course/speed | A second aircraft's peer telemetry over LoRa/ESP-NOW | `POST /peermanager/spoof` |
| Follower's own GPS location | The bench unit's own GPS fix | `POST /gnssmanager/spoof` |
| Follower's own altitude + GCS NAV mode bit | The flight controller (`MSP_ALTITUDE`, `MSP_STATUS`/`MSP_BOXIDS`) | A real (or SITL) INAV FC connected over MSP — this one can't be spoofed from FF's HTTP API |

FF's WiFi AP is reachable at `192.168.4.1` by default (join the
`iNav Radar-<chipid>` SSID it advertises, password `inavradar`) —
`WiFiManager.cpp` doesn't override the ESP default softAP address.

### 1. Spoofing the leader

`POST /peermanager/spoof` (`WiFiManager.cpp:91`) has two modes:

- **No params:** falls back to `PeerManager::enableSpoofing(true)`, which
  generates a fixed synthetic ring of 5 peers 100 m apart around your own
  position — useful for quickly exercising "multiple peers exist" behavior,
  but not for controlled per-preset geometry checks.
- **With `lat`/`lon` (and optional `course`, `speed`, `index`):**
  `PeerManager::spoofPeer()` (`PeerManager.cpp:263`) places exactly one
  synthetic peer at that position, course, and ground speed, and pins it
  there (`spoofOverride[index] = true`) so it doesn't get overwritten by the
  ring generator. This is the repeatable, single-peer tool the plan's
  Phase 0E item added specifically for this kind of bench test.

```bash
# Leader at a known lat/lon, heading due north (0°), doing 5 m/s
curl -d "lat=51.500000&lon=-0.100000&course=0&speed=5" http://192.168.4.1/peermanager/spoof

# Confirm it landed correctly
curl http://192.168.4.1/peermanager/status
```

`speed` is m/s and gets converted to the wire's cm/s internally
(`spoofedPeers[index].gps.groundSpeed = (int16_t)(speed * 100)`), and
`course` is plain degrees, converted to the wire's degrees-×10
(`groundCourse = (int16_t)(course * 10)`) — the same units the HTTP params
take are the same units you'd naturally think in, the conversions happen
inside `spoofPeer()`.

### 1a. Making the leader move on its own (single-aircraft continuous chase)

`spoofPeer()` above is stationary — good for one-shot geometry checks, but
WP#255 just sits at a fixed point, so you never actually watch the follower
*chase* anything. To exercise continuous tracking, freshness, and heading
changes without a second physical aircraft, `PeerManager::spoofPeerHexPath()`
(`PeerManager.cpp:263`) sends a spoofed peer around a closed regular-hexagon
patrol path in real space, independent of your own position:

```bash
# Peer 1 patrols a hexagon with 150 m sides at 8 m/s, centered on wherever
# your own (real or /gnssmanager/spoof'd) GNSS fix currently is
curl -d "index=1&sideLength=150&speed=8" http://192.168.4.1/peermanager/spoof
```

Same `POST /peermanager/spoof` endpoint, keyed off the presence of
`sideLength` (meters per edge) instead of a plain `lat`/`lon`. `lat`/`lon`
are optional here too — supply them to center the hexagon on a specific
point, or omit them and the center defaults to
`GNSSManager::getSingleton()->getLocation()` **at the moment the POST is
handled** (`WiFiManager.cpp`), so you don't need to know or compute
coordinates up front. This only works as expected if your own location is
already set to something real — if you haven't done `/gnssmanager/spoof` or
have no GPS fix yet, `getLocation()` returns `lat=0, lon=0`
(`GNSSManager.cpp:21-44`) and the hexagon centers in the Gulf of Guinea, so
do step 2 below *before* triggering the hex path if you're omitting
`lat`/`lon`.

The peer walks each 60°-stepped edge at constant `speed`, snapping to the
next edge's heading the instant it reaches a vertex — `groundCourse` holds
steady through a leg then jumps by exactly 60° at each corner, and distance
from the center oscillates between the apothem (`sideLength·cos(30°)`, edge
midpoint) and `sideLength` itself (vertex). Confirmed against a Python mirror
of the same math in `mock_server.py` (`HexPath.advance()`/`.position()`):
150 m sides gave a 130–150 m distance envelope and clean 60° course jumps,
matching the geometry exactly.

Altitude also varies, gradually rather than in the horizontal path's discrete
per-vertex jumps: it starts at your own current altitude
(`GNSSManager::getLocation().alt`, captured once, at the moment the hex path
is triggered) **+ 10 m**, climbs *linearly* to a fixed **100 m** at the
halfway point of the loop — the vertex between the 3rd and 4th edge — then
descends linearly back down to that start altitude by the time the loop
closes. It's a "tent" shape over the whole 6-edge loop, not per-edge: climbing
for 3 edges, then descending for the other 3, so you get a slow, continuous
altitude change to test vertical tracking/relalt against, rather than a
teleport. Verified the same way as the horizontal path — driving
`mock_server.py`'s `HexPath.position()` directly at `total_progress`
`0/300/600` m (for 100 m sides) gave altitude `start/peak/start` exactly, with
linear values at the 50 m marks in between.

Poll `GET /peermanager/status` repeatedly to
watch it move — no `MSP`/FC connection needed just to see the peer walk the
path, only to see `FollowManager` react to it (§"Practical bench walkthrough"
below). To stop it, re-`POST` with `lat`/`lon` but no `sideLength` (pins that
index static via `spoofPeer()` instead — this clears the hex path's active
flag); a bare no-param `POST` only flips on the ring generator globally and
does *not* stop an already-running hex path for that index, and there's no
dedicated un-spoof endpoint, so a reboot is the fallback either way.

### 2. Spoofing your own location (easy to forget)

This step is **not optional** for a useful bench test, and it's easy to miss
because nothing in the plan calls it out as its own step. Two separate
pieces of `FollowManager`'s math depend on the follower's own location, via
`GNSSManager::getSingleton()->getLocation()`:

- `FollowManager::targetSane()` (`FollowManager.cpp:213`) calls
  `horizontalDistanceTo(targetLoc)` to enforce `FOLLOW_MAX_TARGET_DIST_M`
  (50 m default). This is computed from your own current location, not the
  spoofed peer's.
- `PeerManager::loop()` (`PeerManager.cpp:174-182`) computes
  `peer->relalt = peerLocation.alt - loc.alt`, where `loc` is again your own
  location — and only recomputes it when `loc.fixType != GNSS_FIX_TYPE_NONE`.

If you spoof a leader at some real-world coordinate but your own GNSS fix is
whatever your bench unit actually has (a weak/no indoor fix, or a real fix
somewhere else entirely), one of two things happens: either `relalt` never
updates (stays 0, silently wrong), or the leader ends up "50+ m away" from
your own position and `targetSane()` permanently refuses to arm — which
looks indistinguishable from a bug in the gate logic if you don't know to
check this first.

Fix it with `/gnssmanager/spoof` (`WiFiManager.cpp:122`), placed near the
spoofed peer:

```bash
curl -d "lat=51.499850&lon=-0.100000" http://192.168.4.1/gnssmanager/spoof
```

This sets `fixType = GNSS_FIX_TYPE_3D`, `alt = 42000` cm (420 m — arbitrary
but fixed and known), `numSat = 42`, and `hdop = 0.69`
(`WiFiManager.cpp:129-135`), and makes `GNSSManager::getLocation()` return
this fixed value on every call until you restart the device — there's no
un-spoof endpoint, so a reboot is currently the only way back to a real fix.

### 3. Altitude and the GCS NAV gate — needs a real or SITL FC

Unlike the two above, these genuinely cannot be faked from FF's HTTP API:

- `MSPManager::local_altitude_cm()` (`MSPManager.cpp:148`) polls
  `MSP_ALTITUDE` (109) from whatever's on the other end of FF's MSP
  connection and caches `estimatedActualPosition` — this is the FC's own
  baro/GPS-fused home-relative altitude estimate. There's no FF-side
  override for it.
- `FollowManager::followSwitchActive()` (`FollowManager.cpp:57`) — with the
  shipped default `FOLLOW_TRIGGER_MODE = FOLLOW_TRIGGER_GCSNAV` — gates on
  `MSPManager::isGCSNavActive()`, which reads bit 23
  (`MSP_MODE_GCSNAV`) out of `MSP::getActiveModes()`
  (`MSPManager.cpp:41-49`). That bit only exists on the FC. (The
  `FOLLOW_TRIGGER_AUX` alternative in `FollowConfig.h` is a placeholder —
  `followSwitchActive()` always returns `false` for it today; it's listed as
  optional/stretch Phase 2b in the plan and hasn't been implemented.)

So for anything past pure geometry-formula checking, you need something on
the other end of FF's MSP UART that speaks real INAV MSP:

- **A real INAV flight controller, props removed**, wired to FF over its
  normal MSP connection. Arm it (props off!) and switch on `NAV POSHOLD` +
  `GCS NAV` from your transmitter, same as you'd do to test any other nav
  mode on the bench. This is the most direct option and matches exactly what
  the spec's bench checklist (§12.1) assumes.
- **INAV SITL** (software-in-the-loop), if you want to remove the physical
  aircraft from the loop entirely. INAV's SITL target implements the same
  MSP surface (`MSP_ALTITUDE`, `MSP_STATUS`/`MSP_BOXIDS`, `MSP_SET_WP`) a
  real FC does, so FF can't tell the difference. This is genuinely
  zero-hardware-on-the-follower-side testing, but the exact serial/TCP
  bridging setup is an INAV-side concern — check INAV's own SITL
  documentation for how to expose it as something FF's MSP connection can
  reach, since that detail is outside this codebase and may change with
  INAV versions.

Either way, **watch what mode bits are actually active** rather than
assuming your switch did what you think — `MSPManager::getState()`
already reads the ARM bit via the same `getActiveModes()` call, and INAV
Configurator's "Status" tab that FF is emulating shows exactly which nav
modes it believes are active in real time.

## Watching FollowManager's output without a live status endpoint

As of Phase 1 (current `master`/`follow_on_inav`), `GET /followmanager/status`
does **not exist yet** — it's specified for Phase 2, which hasn't landed.
There's no FF-side JSON to poll for `PeerLock` state or the last computed
target. Until that lands, verify behavior externally:

- **INAV Configurator's Mission Control / WP list tab**, pointed at the same
  FC/SITL FF is talking to. Waypoint #255 updates live at
  `FOLLOW_EMIT_HZ` (4 Hz default) while the gate is active — refresh the WP
  list and watch its lat/lon/alt change to track your spoofed leader.
- **`MSP_WP` (#254)** read back directly if you're scripting against the FC's
  MSP port yourself, rather than going through Configurator.
- **The OSD**, if the FC is feeding one, showing nav target / home-relative
  altitude.

If you're doing repeated development-loop testing and the Configurator
round-trip is too slow, a temporary `DBGF`/`DBGLN` (see other `MSPManager.cpp`
call sites for the pattern) at the end of `FollowManager::loop()` printing
the computed `target`/`altCm` over FF's serial/USB debug output is a
reasonable stand-in until Phase 2's status endpoint exists — just don't ship
it.

## Cross-checking the geometry by hand

Because `slotToLatLon()` (`FollowManager.cpp:12`) is pure math with no
hardware dependency, you can independently compute the expected target and
compare it against what actually gets emitted — this is how you catch a
scaling or sign error rather than just eyeballing "it looks about right on
the map."

The projection, in the leader's track-relative frame (spec §7.1–§7.2):

```
north_m = long_m·cos(course) − lat_m·sin(course)
east_m  = long_m·sin(course) + lat_m·cos(course)
distance = √(north_m² + east_m²)
bearing  = atan2(east_m, north_m)   (normalized to 0–360°)
```

...then a standard great-circle projection from the leader's lat/lon at that
`distance`/`bearing` (same formula as `GNSSManager::calculatePointAtDistance`,
`GNSSManager.cpp:174`).

Worked example, default chase-high preset (`BEHIND`/`CENTER`/`ABOVE`,
`FOLLOW_GAP_LONG_M = 15`, `FOLLOW_GAP_VERT_M = 10`, so `long_m = -15`,
`lat_m = 0`) with a leader at `51.500000, -0.100000` heading `course = 0°`:

- `north_m = -15·cos(0) − 0·sin(0) = -15`, `east_m = -15·sin(0) + 0·cos(0) = 0`
- `distance = 15 m`, `bearing = atan2(0, -15) = 180°` (due south — "behind" a
  north-heading leader is south of it, as expected)
- Projecting 15 m south from `51.500000, -0.100000` lands at approximately
  `51.499865, -0.100000` (Δlat ≈ 15 / 111320 ≈ 0.000135°, longitude
  unchanged at due-south bearing)

Altitude: `alt_cm = local_altitude_cm() + peer->relalt×100 + vertical_m×100`.
With the leader spoofed to `alt = 100` (fixed in `spoofPeer()`,
`PeerManager.cpp:274`, in meters) and your own spoofed `alt = 42000` cm =
420 m, `peer->relalt = 100 − 420 = −320` m, so a follower FC reporting e.g.
`local_altitude_cm() = 0` (on the ground, home altitude) would command
`alt_cm = 0 + (−320×100) + (10×100) = −31000` cm — a deliberately
unrealistic combination in this made-up example, useful only to confirm you
understand which numbers combine which way. For an actually sane bench
check, pick your `/gnssmanager/spoof` altitude and `/peermanager/spoof`
implied altitude (fixed at 100 m by `spoofPeer()` — not currently a
settable HTTP param) so `relalt` comes out to something realistic, or just
confirm the *offset* is being added with the correct sign relative to
whatever the FC reports rather than chasing an absolute number.

If you need the peer's simulated altitude to be something other than the
hardcoded 100 m in `spoofPeer()`, that's a small extension in the same
shape as the existing `lat`/`lon`/`course`/`speed` params — not currently
exposed.

## Practical bench walkthrough (maps to spec §12.1)

1. Flash FF onto your bench unit; connect it to a real (or SITL) INAV FC
   over MSP with props removed; join the FF WiFi AP from your laptop/phone.
2. `POST /gnssmanager/spoof` with a fixed lat/lon near where you'll place the
   leader (§2 above) — do this first, since the sanity guard depends on it.
3. `POST /peermanager/spoof` with a `lat`/`lon` offset from your spoofed own
   position by roughly the geometry preset you're testing, plus a `course`
   and `speed` above `FOLLOW_MIN_COURSE_SPEED` (2 m/s default). For a
   continuous chase test instead of a single fixed target, use `sideLength`
   (§1a above) to send the peer around a hexagon patrol path instead.
4. `GET /peermanager/status` to confirm the peer shows up with the right
   `lat`/`lon`/`groundCourse`/`groundSpeed`, `lost: 0`.
5. Arm the FC (props off) and switch on `NAV POSHOLD` + `GCS NAV`.
6. Watch Configurator's WP#255 update at 4 Hz; compare against your
   hand-computed target from the previous section.
7. Kill the spoofed peer's updates (stop calling `/peermanager/spoof` for
   that index, or don't refresh it) and confirm WP#255 stops updating within
   `FOLLOW_PEER_TIMEOUT_MS` (1500 ms default) — this is the freshness guard.
8. Toggle `GCS NAV` off — confirm emission stops immediately
   (`followSwitchActive()` returning `false` resets `PeerLock` to `IDLE`,
   `FollowManager.cpp:258-265`).
9. Re-spoof the peer below `FOLLOW_MIN_COURSE_SPEED` (e.g. `speed=1`) and
   confirm the `HOLD_COURSE` stationary fallback holds the last valid course
   rather than snapping to whatever a near-zero `groundCourse` reports
   (spec §7.5) — this is the cm/s-vs-m/s comparison the plan calls out as
   easy to get silently wrong, so it's worth deliberately bracketing around
   the 2 m/s threshold (try `speed=1.5` and `speed=3`) rather than just
   testing one side.
10. Try a preset that violates `FOLLOW_MIN_SEP_M`/`FOLLOW_MIN_VSEP_M` (e.g.
    build with tiny gap values) and confirm `targetSane()` refuses to arm —
    no WP#255 update at all, rather than an unsafe one.

None of this requires props on, a GPS fix outdoors, or a second physical
aircraft.

## What still needs a real flight

Bench testing validates the math, the state machine, and the MSP wiring —
it does not validate:

- Real GPS accuracy/noise on both aircraft, which is exactly what
  `FOLLOW_MIN_VSEP_M`'s 13 m default (5 m physical clearance + 8 m assumed
  GPS vertical error) is trying to absorb (spec's "Unit and altitude-frame
  correctness" section) — the plan explicitly calls for a two-real-GPS-units
  check at a known height difference before trusting that margin.
- Whether the follower's flight controller actually captures and holds the
  commanded waypoint in the air — POSHOLD/nav-to-waypoint control loop
  behavior is INAV's, not FF's, and a spoofed bench test can't exercise it.
- LoRa/ESP-NOW link margin and peer staleness under real RF conditions
  instead of an HTTP call that never has to contend with actual link loss.

Keep the bench tests as the fast, safe, repeatable first pass, and follow
the spec's progressive flight checklist (§12.2) — trail slot, generous
gaps, low speed, manual-override tested first — once the bench checklist
above passes cleanly.
