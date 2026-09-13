# FormationFlight v2 web API

The contract between the firmware (`src/hal/WebServer`), the web UI (`html/`)
and the development mock server (`scripts/mock_server.py`). All three are
written against this document, and `test/test_mock_server.py` plus the native
config tests check that the validation rules agree.

Everything is JSON over HTTP on port 80. There is no websocket: an ESP8285 with
a LoRa radio, an MSP link and a web server running has around 25 KB of heap to
spare, and a websocket's per-client buffers were not worth the polling it saves.
The UI polls, round-robin, one endpoint per tick.

Paths are versioned by the firmware, not by the URL. A field the UI does not
recognise is ignored; a field the firmware does not recognise is ignored. That
is deliberate, so a newer UI against older firmware degrades rather than breaks.

## Conventions

- UIDs are lower-case hex strings, 8 characters, no prefix: `"1a2b3c4d"`. Every
  UID in the API, config included, uses this form, so a UID copied off the
  dashboard can be pasted straight into a setting. `"00000000"` is the
  "no peer" / "pick one for me" sentinel. On the way in, 1 to 8 hex digits are
  accepted and a bare JSON number still works, so a hand-edited config file is
  easy to get right.
- Latitude and longitude are integers in degrees x 1e7, matching the wire
  protocol and MSP. Altitude is metres, speed cm/s, course decidegrees.
- Times are milliseconds. `*_ms` on its own is a duration or an uptime;
  `age_ms` is how long ago something happened.
- Errors are `4xx` with a `text/plain` body naming the offending field. The UI
  shows that string verbatim, so it has to be worth reading.

## GET /api/status

Everything the dashboard shows. Roughly 2 KB with a full peer table.

```json
{
  "node": {
    "uid": "1a2b3c4d", "name": "A1B", "version": "v2.0.0-12-gabc1234",
    "uptime_ms": 91234, "free_heap": 24160, "listen_only": false
  },
  "location": {
    "valid": true, "source": "msp", "lat": 370000000, "lon": -1220000000,
    "alt_m": 120, "speed_cms": 1500, "course_ddeg": 900, "armed": false,
    "sats": 12, "fix_type": 3, "hdop": 1.31
  },
  "radios": [
    {
      "index": 0, "name": "ESPNOW", "enabled": true, "sim": false,
      "tx": 412, "rx_ok": 389, "rx_crypto_fail": 0, "rx_replay": 0,
      "rx_decode_fail": 0, "rx_self": 412, "last_rssi": -52,
      "last_rx_age_ms": 84, "beacon_interval_ms": 100, "airtime_ms": 0.4,
      "peers": 2, "rx_dropped": 0, "tx_dropped": 0, "transmits": true
    },
    {
      "index": 1, "name": "LORA", "enabled": true, "sim": false,
      "tx": 51, "rx_ok": 48, "rx_crypto_fail": 1, "rx_replay": 0,
      "rx_decode_fail": 0, "rx_self": 2, "last_rssi": -66,
      "last_rx_age_ms": 210, "beacon_interval_ms": 816, "airtime_ms": 61.2,
      "peers": 1, "rx_dropped": 2, "tx_dropped": 3, "transmits": true,
      "modulation": { "frequency_hz": 920000000, "bandwidth_khz": 500.0,
        "spreading_factor": 8, "coding_rate": 7, "power_dbm": 10 },
      "last_snr_db": 9.75
    }
  ],
  "peers": [
    {
      "uid": "aabbccdd", "name": "XYZ", "lat": 370001000, "lon": -1220001000,
      "alt_m": 130, "speed_cms": 1400, "course_ddeg": 880, "flags": 3,
      "rssi": -71, "age_ms": 120, "packets": 830, "radios": [0, 1],
      "distance_m": 143.2, "bearing_deg": 47, "rel_alt_m": 10
    }
  ],
  "crypto": { "mode": "ccm", "bad_tag": 0, "replay": 0, "tx_counter": 412 },
  "stats": {
    "beacons_sent": 412, "announces_sent": 45, "rx_ok": 389,
    "rx_rejected": 2, "rx_self": 412
  },
  "fc": {
    "connected": true, "variant": "INAV", "version": "9.1.0",
    "platform": 1, "armed": false, "gcs_nav": true, "heading_hold": false
  },
  "follow": {
    "state": "LOCKED", "gate_active": true, "locked_uid": "aabbccdd",
    "locked_name": "XYZ", "target": { "lat": 370000500, "lon": -1220000500,
    "alt_cm": 13000, "heading_deg": 88, "age_ms": 210 },
    "status_gvar": 2, "condition_gvar": 0, "platform": 1,
    "autothrottle_armed": true, "autothrottle_engaged": false,
    "target_speed_cms": 0, "rc_slot_frozen": false,
    "live_offset": { "long_m": -15, "lat_m": 0, "vert_m": 10 },
    "prearm_failed": false,
    "prearm_offset": { "long_m": -15, "lat_m": 0, "vert_m": 10 }
  },
  "sim": { "enabled": false, "peers": 0 },
  "power": {
    "battery_v": 4.02, "supply_v": 5.05, "charge_ma": 240.0,
    "discharge_ma": 0.0, "pmic_temp_c": 31.5, "battery_present": true,
    "charging": true, "usb_present": true, "battery_pct": 87
  },
  "wifi": { "mode": "ap", "channel": 1, "configured_channel": 1, "ap_clients": 0 },
  "system": {
    "cpu_mhz": 240, "free_heap": 24160, "sketch_size": 962192,
    "free_sketch_space": 1310720, "reset_reason": "power on",
    "largest_free_block": 19328, "flash_size": 4194304,
    "min_free_heap": 23904
  },
  "loop": {
    "last_us": 346, "min_us": 114, "max_us": 212868, "mean_us": 348,
    "rate_hz": 2873, "samples": 201141, "overruns": 3,
    "overrun_threshold_us": 100000
  },
  "log": { "total": 44, "warnings": 8, "errors": 1 },
  "reboot_required": false,
  "config_corrupt": false
}
```

`reboot_required` goes true once a setting was changed that only takes effect at
boot: the group passphrase, or enabling a radio that was off when the node
started. `config_corrupt` means the stored config file existed but could not be
parsed or did not validate, so the node is running on defaults and the file has
been left alone for recovery. The UI should say both out loud.

`fc.platform` is INAV's mixer platform type: 0 multirotor, 1 airplane, 255 not
yet answered.

Absent is not the same as zero, and the firmware omits rather than zero-fills:

- `follow.target`, `follow.live_offset`, `follow.autothrottle_engaged` and
  `follow.target_speed_cms` appear only once a target has actually been solved.
  Zero is a legitimate offset and must not be confused with "no data".
- `follow.prearm_offset` appears only while the pre-arm check is running, which
  needs the craft disarmed with at least one RC axis assigned.
- `follow.status_gvar` and `follow.condition_gvar` appear only while that GVAR
  slot is enabled and has been sent at least once.
- `peers[].distance_m`, `bearing_deg` and `rel_alt_m` appear only when this node
  has its own fix; without one there is nothing to measure from.
- `crypto` carries its counters only when the cipher is on. With it off the
  object is just `{"mode": "none"}`. `mode` is `"ccm"` or `"none"`.
- `radios[].last_rssi` and `peers[].rssi` are `0` when there is no reading,
  rather than absent: ESP-NOW and the virtual radio report no signal level at
  all. Zero is not a plausible RSSI in dBm, so it is unambiguous, but it is the
  one place this API uses a sentinel instead of omitting the field.
- `radios[].modulation` appears only for a driver that reports a frequency.
  ESP-NOW and the virtual radio have none, so the object is absent for them
  rather than present and full of zeros.
- `radios[].last_snr_db` appears only for a driver that measures SNR - the LoRa
  drivers, and only once one has actually received a frame. 0 dB is a perfectly
  ordinary SNR, so it could not double as "no reading".
- `location.hdop` appears only when the position source reports one. It is a
  float (the wire value is x100 and the firmware divides). A source that does
  not report it omits the field rather than publishing a flawless `0.0`.
- `power` appears only on a board whose PMIC answered, and `power.battery_pct`
  only when that PMIC will estimate one. "0%" and "no estimate" are very
  different things to put in front of a pilot.
- `loop` appears only on a build with loop timing wired to the web server. Every
  shipping target has it; a bare host build may not.

`follow.locked_uid` and `follow.locked_name` are always present, reading
`"00000000"` and `""` when nothing is locked.

`radios[].sim` marks the virtual radio that simulated traffic arrives on, so the
UI can make it obvious the numbers are not real RF.

`peers[].flags` is the wire status bitmask from `ff::PositionFlags`: bit 0 (`1`)
the peer reports itself armed, bit 1 (`2`) it has a GPS fix. A peer without a
fix is tracked and displayed but is never followable.

`radios[].rx_dropped` and `tx_dropped` count frames lost *inside the node*
rather than on the air: receive because the driver's ring filled before the loop
drained it, transmit because the radio was still busy with the previous frame.
Neither shows up in any other counter, and either one climbing means the node is
over its budget.

`radios[].transmits` is false for a receive-only driver, which the simulated
radio is: it injects frames and never keys an antenna.

`radios[].modulation` is read back from the driver, not from the build flags or
the config, and that is the whole point of it. On a node that is not hearing
anyone, confirming the radio really is on the frequency and modulation the
target intended is the first thing worth checking and the hardest thing to see
any other way. Note that `power_dbm` is the driver's configured transmit power
(the `LORA_POWER` build flag), which is not the same field as
`radios.lora_power_dbm` in the config and can legitimately differ from it.
`coding_rate` is the denominator: `7` means 4/7. `bandwidth_khz` is a float.

### Fix quality

`location.sats` and `location.fix_type` say how good the fix under
`location.lat`/`lon` actually is. "No fix" and "a four-satellite fix wandering by
30 m" are very different problems and look identical without them.

`fix_type` uses UBX's NAV-PVT numbering, and that is the only scheme the API
publishes - every `location.source` reports it the same way:

| value | meaning |
| --- | --- |
| `0` | no fix |
| `1` | dead reckoning only |
| `2` | 2D fix |
| `3` | 3D fix |
| `4` | 3D fix + dead reckoning |
| `5` | time only |

A `"gnss"` source carries the receiver's own `fixType` byte through, except that
a solution the receiver does not trust (its `gnssFixOK` flag clear) is reported
as `0` rather than at the quality it claims. An `"msp"` source is converted on
the way in - `MSP_RAW_GPS` numbers the same states `0`/`1`/`2`, and the firmware
maps them to `0`/`2`/`3` before the value reaches the API - so a reader never
sees MSP's numbering. Treat `3` (or `4`) as a 3D fix and `2` as 2D.

`location.hdop` is horizontal dilution of precision as a float - the firmware
divides the x100 wire value before sending it - and is omitted entirely when the
source does not report one.

### Board power

`power` is present only on a board with a power-management IC that answered
(the T-Beam's AXP192). Its absence on such a board is itself the diagnostic: the
GPS and LoRa rails are unpowered. Boards without a PMIC never send it, so the UI
has to read an absent object as "no PMIC", never as zero volts.

| field | meaning |
| --- | --- |
| `battery_v` | battery terminal voltage |
| `supply_v` | USB / external supply voltage, `0` when there is none |
| `charge_ma` | current *into* the battery |
| `discharge_ma` | current *out of* the battery |
| `pmic_temp_c` | the PMIC's own die temperature, not ambient |
| `battery_present` | a battery is attached |
| `charging` | the PMIC is charging it right now |
| `usb_present` | external power is connected |
| `battery_pct` | 0-100 estimate, **omitted** when the PMIC will not make one |

Charge and discharge are separate readings, which is what makes a node on USB
with a battery attached show a supply voltage *and* a charge current at the same
time. That is the state people most often misread.

### System

`system` is always present and is the "why is this board behaving like this"
block. `reset_reason` is the most useful field in it and is invisible everywhere
else: it is a short string like `"power on"`, `"software restart"`,
`"panic or exception"`, `"brownout"`, `"task watchdog"` or `"unknown"`.

`cpu_mhz`, `free_heap`, `sketch_size`, `free_sketch_space`,
`largest_free_block` and `flash_size` are always there. `free_heap` is the same
reading as `node.free_heap`.

Two fields are platform-specific, and a node sends one or the other, never both:

- `heap_fragmentation_pct` (ESP8266 only). It matters more than free heap on an
  8266: a web request can fail for want of one contiguous block while plenty of
  heap is nominally free. `largest_free_block` is the other half of that story.
- `min_free_heap` (ESP32 only): the low-water mark since boot, which catches a
  transient the UI's polling would otherwise never see.

The development mock sends both, so the UI path for each is reachable without
that board on the bench. That is a property of the mock, not of the contract.

### Loop timing

`loop` is main-loop timing, from `ff::LoopStats`. ALOHA tolerates jitter by
design, which is exactly why this is worth publishing: the firmware keeps
working as it gets slower, right up until it does not.

- `last_us`, `min_us`, `max_us`, `mean_us` - one iteration's duration in
  microseconds. **`max_us` is the number that matters.** A 2 ms mean with a
  300 ms peak is a node with a problem, and the mean alone looks healthy.
- `rate_hz` - loops per second, derived from the mean.
- `samples` - iterations measured.
- `overruns` - iterations longer than `overrun_threshold_us`.
- `overrun_threshold_us` - the threshold, set at boot from the shortest beacon
  interval (`rate.min_interval_ms`). A loop longer than that can miss a
  transmission outright, which no on-air counter would explain.

`min_us`, `mean_us` and `rate_hz` read `0` before the first sample.

`loop` is absent on a build with no loop timing wired to the web server.

### Log counters

`log` in the status document is counters only - `total`, `warnings` and
`errors`, all since boot. The entries themselves are `GET /api/log`. A node that
logged an error an hour ago and has scrolled past it still says so here, which
is what makes the counters worth polling separately from the log view.

### WiFi

`wifi.mode` is `"ap"` or `"ap_sta"`; in `ap_sta` the object also carries
`sta_connected` and `sta_rssi`. `channel` is the channel the radio is actually
on and `configured_channel` is the one `wifi.channel` asked for. **They differ
when the node has joined an external network, because the router owns the
channel then.** That matters far more than it looks: ESP-NOW rides whatever
channel the WiFi radio is on, so two nodes on different channels cannot hear
each other over ESP-NOW while both still report themselves perfectly healthy.
It is the single most confusing failure this firmware can produce, which is why
both numbers are published.

## GET /api/config

The full configuration, exactly as `lib/ff_core/config.h` defines it. Secrets
(`security.passphrase`, `wifi.psk`, `wifi.ap_psk`) come back as `"••••••••"`
when set and `""` when not. Posting that placeholder back means "leave it
alone", so the UI can round-trip the document it was given.

`rate.duty_cycle_pct` is the regional transmit duty-cycle ceiling, 0 for none.
It is seeded per band at build time and is a hard floor on the beacon interval,
outranking both `min_interval_ms` and `max_interval_ms`: it is a legal limit
rather than a tuning preference, so it is not something a config is allowed to
clamp away. On EU 868 at roughly 72 ms of airtime and 10% duty it works out at a
beacon every 720 ms no matter what else is set.

`wifi.channel` (1-13) sets the AP's channel and therefore ESP-NOW's. Every node
that should hear every other over ESP-NOW must agree on it. 1, 6 and 11 are the
non-overlapping choices.

## POST /api/config

Body is a partial config. Only the keys present are applied, then the whole
result is validated; if validation fails nothing changes and the response is
`400` with the message. On success the response is the new config, same shape as
the GET.

This is a RAM-only change and takes effect immediately. It is not persisted
until `/api/config/save`, so a setting that makes the node unreachable can be
recovered with a power cycle.

```
POST /api/config
{"follow": {"ofsLongM": -20}, "rate": {"target_load": 0.2}}
```

## POST /api/config/save

Writes the live configuration to `/config.json`. Empty body. `200` on success,
`5xx` with a message if the filesystem write failed, and `429` if the last save
was under two seconds ago. Flash has a finite number of erase cycles and a Save
button has an infinite number of clicks.

The write goes to a temporary file and is renamed into place, so an interrupted
save leaves either the old config or the new one, never a truncated one.

## POST /api/config/reset

Restores compile-time defaults and saves them. Empty body, `200` with the body
`reset`. The node keeps running on the new config and does not reboot itself,
because the caller may want to set a few fields before the radio comes back up
on a different group. `reboot_required` goes true.

## GET /api/frames

The frame log, newest first. Optional `?since=N` returns only frames logged
after total counter N, so a polling client can ask for what it missed.

```json
{
  "total": 1834,
  "capacity": 32,
  "frames": [
    { "ms": 91180, "radio": 0, "uid": "aabbccdd", "type": 1, "len": 31,
      "rssi": -71, "result": "ok" }
  ]
}
```

`result` is one of `tx`, `ok`, `self`, `crypto_fail`, `replay_fail`,
`decode_fail`, `oversize`. `type` is 1 for a position beacon, 2 for an
announce, 0 when the frame never decoded far enough to tell.

Entries carry no sequence number of their own. They are newest-first, so entry
`i` in the array has sequence `total - 1 - i`; that is what `since` is compared
against. `total` is the count since boot and keeps climbing past `capacity`, so
a client whose `since` is further behind than the ring is deep has missed
frames, and should say how many rather than pretending the list is complete.

## GET /api/log

The node's in-RAM log, newest first. On an ESP8266 target this is the **only**
safe log there is: the console UART is the MSP UART, so printing a diagnostic
there injects bytes into the flight controller's serial link. The usual answer,
print and watch the console, is actively harmful on the boards most likely to
need debugging, which is why this endpoint exists.

```json
{
  "total": 1042,
  "capacity": 48,
  "warnings": 8,
  "errors": 1,
  "entries": [
    { "ms": 91180, "level": "warn", "text": "LoRa tx dropped: radio still busy" },
    { "ms": 89210, "level": "info", "text": "peer aabbccdd XYZ seen on LORA" }
  ]
}
```

`level` is one of `debug`, `info`, `warn`, `error`. `ms` is node uptime when the
line was recorded. `text` is already truncated to what the ring can hold (72
characters); a line long enough to be cut is one that should have been shorter.

The ring is fixed-size and the oldest entry is overwritten, so `entries` never
exceeds `capacity`. `warnings` and `errors` are counts **since boot**, not counts
of what is currently in the ring: a node that logged an error and has long since
scrolled past it still reports it.

`?since=N` works exactly like `/api/frames`. Entries carry no sequence number of
their own; they are newest-first, so entry `i` in the array has sequence
`total - 1 - i`, and the server stops emitting as soon as it reaches one below
`N`. A client polls with `since` set to the `total` it last saw. `since=0`, like
an absent `since`, returns the whole ring.

`total` counts everything ever logged and **never goes backwards**, including
across a clear. A client whose `since` is further behind than the ring is deep
has missed lines and should say how many rather than pretending the list is
complete.

## DELETE /api/log

Empties the ring. `200` with the body `cleared`.

It deliberately does **not** reset `total`, `warnings` or `errors`. Those are
since-boot counters, and resetting `total` would send every outstanding client
cursor backwards, asking for entries that no longer exist. "This node has hit an
error since it booted" also does not stop being true because somebody pressed a
button in a web page. Only a reboot clears them.

## Simulated traffic

`GET /api/sim` always answers, so the UI can discover whether the simulator is
on. Adding peers requires `sim.enabled`; removing them does not. The simulator
injects
frames into a virtual radio, through the same decrypt and decode path real RF
takes, so everything downstream (peer table, Follow, MSP radar output) cannot
tell the difference. That is the point: it exercises the real code, not a mock
of it.

### GET /api/sim

Always answers, even with the simulator off: it is how the UI learns whether it
is on. Note that `lat` and `lon` here are decimal degrees, unlike everywhere
else in this API, because they are the values a human typed into the form rather
than anything that came off the wire.

```json
{ "enabled": true, "peers": [ { "uid": "5eed0001", "name": "SIM1",
  "mode": "hex", "lat": 37.0, "lon": -122.0, "alt_m": 120,
  "speed_ms": 15, "course_deg": 90, "radius_m": 150,
  "elapsed_ms": 42000, "running": true } ] }
```

### POST /api/sim/peer

Creates or replaces one simulated peer, and restarts its path from the moment of
the call. This is a replace, not a merge: any key you omit takes its default, so
send the whole peer. `uid` is optional and is generated if absent.

Requires `sim.enabled` to already be true, otherwise `409`. At most four
simulated peers exist at once; a fifth is `409`. `200` with the body `ok`.

```json
{ "uid": "5eed0001", "name": "SIM1", "mode": "hex", "lat": 37.0,
  "lon": -122.0, "alt_m": 120, "speed_ms": 15, "course_deg": 90,
  "radius_m": 150 }
```

`mode` is one of:

- `static` - sits at `lat`/`lon`/`alt_m`, reporting `course_deg` and zero speed.
- `line` - travels from `lat`/`lon` along `course_deg` at `speed_ms` forever.
- `circle` - orbits `lat`/`lon` at `radius_m` and `speed_ms`.
- `hex` - flies a closed hexagon of side `radius_m` centred on `lat`/`lon`,
  climbing to a peak at the half-way vertex and descending back. The awkward
  one, and therefore the useful one: heading changes in steps, altitude ramps,
  and the loop closes, so a follower has to cope with all three.

`lat` and `lon` here are decimal degrees, not 1e7 integers, for the same reason
as the GET above.

### DELETE /api/sim/peer?uid=...

Removes one simulated peer. `POST /api/sim/clear` removes all of them.

## POST /api/system/reboot

Empty body, `200`, then the node reboots after a short delay so the response
makes it out.

## POST /update

Firmware upload, multipart form, unchanged from v1. The response body is the
result string; the node reboots on success. An ESP8266 image may be `.bin` or
`.bin.gz`; an ESP32 image must be `.bin`. Anything else is rejected on the first
chunk, before a single byte is written to flash.

There is no progress endpoint. A browser client should read upload progress from
`XMLHttpRequest.upload`, which is why the UI does not use `fetch` here.
