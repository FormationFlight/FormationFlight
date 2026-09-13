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
    "alt_m": 120, "speed_cms": 1500, "course_ddeg": 900, "armed": false
  },
  "radios": [
    {
      "index": 0, "name": "ESPNOW", "enabled": true, "sim": false,
      "tx": 412, "rx_ok": 389, "rx_crypto_fail": 0, "rx_replay": 0,
      "rx_decode_fail": 0, "rx_self": 412, "last_rssi": -52,
      "last_rx_age_ms": 84, "beacon_interval_ms": 100, "airtime_ms": 0.4,
      "peers": 2, "rx_dropped": 0, "tx_dropped": 0
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
  "wifi": { "mode": "ap", "channel": 1, "configured_channel": 1, "ap_clients": 0 },
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
