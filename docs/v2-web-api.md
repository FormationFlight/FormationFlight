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

- UIDs are lower-case hex strings, 8 characters, no prefix: `"1a2b3c4d"`. They
  are 32-bit values but JSON numbers are doubles, and a UID that round-trips
  through a double is a UID that can come back wrong.
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
    "alt_m": 120, "speed_cms": 1500, "course_ddeg": 900, "armed": false
  },
  "radios": [
    {
      "index": 0, "name": "ESPNOW", "enabled": true, "sim": false,
      "tx": 412, "rx_ok": 389, "rx_crypto_fail": 0, "rx_replay": 0,
      "rx_decode_fail": 0, "rx_self": 412, "last_rssi": -52,
      "last_rx_age_ms": 84, "beacon_interval_ms": 100, "airtime_ms": 0.4,
      "peers": 2
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
  "sim": { "enabled": false, "peers": 0 }
}
```

`fc.platform` is INAV's mixer platform type: 0 multirotor, 1 airplane, 255 not
yet answered. `follow.target`, `follow.live_offset` and `follow.prearm_offset`
are absent when the corresponding value has never been computed, rather than
present and zero: zero is a legitimate offset and must not be confused with "no
data". Same for `follow.status_gvar` and `follow.condition_gvar`, which are
absent while that GVAR slot is disabled.

`radios[].sim` marks the virtual radio that simulated traffic arrives on, so the
UI can make it obvious the numbers are not real RF.

## GET /api/config

The full configuration, exactly as `lib/ff_core/config.h` defines it. Secrets
(`security.passphrase`, `wifi.psk`, `wifi.ap_psk`) come back as `"••••••••"`
when set and `""` when not. Posting that placeholder back means "leave it
alone", so the UI can round-trip the document it was given.

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
`5xx` with a message if the filesystem write failed.

## POST /api/config/reset

Restores compile-time defaults and saves them. Empty body. The node keeps
running on the new config; it does not reboot itself, because the caller may
want to set a few fields before the radio comes back up on a different group.

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

`total` is the count since boot, which exceeds `capacity`; a client whose
`since` is further behind than the ring is deep has missed frames and should say
so rather than pretending the list is complete.

## Simulated traffic

Only present when `sim.enabled` is true in the config. The simulator injects
frames into a virtual radio, through the same decrypt and decode path real RF
takes, so everything downstream (peer table, Follow, MSP radar output) cannot
tell the difference. That is the point: it exercises the real code, not a mock
of it.

### GET /api/sim

```json
{ "enabled": true, "peers": [ { "uid": "5eed0001", "name": "SIM1",
  "mode": "hex", "lat": 370000000, "lon": -1220000000, "alt_m": 120,
  "speed_ms": 15, "course_deg": 90, "radius_m": 150, "running": true } ] }
```

### POST /api/sim/peer

Creates or updates one simulated peer. `uid` is optional on create and
generated if absent.

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

`lat` and `lon` here are decimal degrees, not 1e7 integers, because they are
typed by a human into a form.

### DELETE /api/sim/peer?uid=...

Removes one simulated peer. `POST /api/sim/clear` removes all of them.

## POST /api/system/reboot

Empty body, `200`, then the node reboots after a short delay so the response
makes it out.

## POST /update

Firmware upload, multipart form, unchanged from v1. The response body is the
result string; the node reboots on success.
