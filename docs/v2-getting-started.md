# Getting started with FormationFlight v2

What you need on day one: flash a node, reach its web UI, put your aircraft in
the same group, and confirm they can see each other.

> **Status.** No part of v2 has been validated on real hardware yet. Builds are
> compile-verified and the pure logic is covered by host tests, but nothing
> below has been flown. Treat first power-on as bring-up, not as a known-good
> procedure.

v2 does not interoperate with v1. A v2 node and a v1 node cannot see each
other at all: different packet format, different cipher. Flash every aircraft
in the group.

## 1. Flash the firmware

Pick the PlatformIO environment matching your board and radio, then build and
upload over USB:

```bash
pio run -e expresslrs_rx_2400_via_UART -t upload
```

The environment names live in `targets/*.ini`. See
[`explainers/building-the-firmware.md`](explainers/building-the-firmware.md)
for the full list and for the OTA (`_via_WiFi`) variants. A node already
running v2 can also be updated from its own web UI: the *Update* page takes a
`firmware.bin` and reboots itself when the write completes.

You do not need to upload a filesystem image. The node mounts LittleFS at
boot, formats it only if it cannot be mounted at all, and writes a default
`/config.json` the first time it finds none.

## 2. Join the node's WiFi

On boot the node brings up its own access point:

- SSID: `FormationFlight-<uid>`, where `<uid>` is the node's 32-bit UID as 8
  hex characters, for example `FormationFlight-1a2b3c4d`. Each node's SSID is
  different, which is how you tell several on a bench apart without connecting
  to each in turn.
- Password: none. The AP is open until you set one under *Settings > WiFi >
  AP password*. WPA2 refuses anything shorter than 8 characters, and so does
  the node, rather than silently coming up open.

Then browse to <http://192.168.4.1/>. The sidebar has *Dashboard*, *Radios*,
*Follow*, *Settings*, *Simulator* and *Update*.

If you would rather have the node join an existing network, set *Settings >
WiFi > Run own AP* off and fill in the SSID and password. That change only
takes effect at the next boot, and getting it wrong is how you lose the web
UI: use *Apply*, confirm the node is still reachable, and only then *Save*.

## 3. Set a group passphrase

Every frame on the air is encrypted with AES-128-CCM. The key is derived from
a passphrase, and nodes only see each other if they derive the same key.

Under *Settings > Node > Security*, set *Group passphrase* to the same string,
up to 32 characters, on every aircraft in your group. Three cases:

| Passphrase | What you get |
|---|---|
| Empty | The default key. It is public and compiled into every build, so traffic is encrypted and authenticated but not secret. Two fresh nodes talk to each other out of the box. |
| Anything else | A group key nobody else has. Only nodes with the same passphrase decode your traffic. |
| The literal word `none` | The cipher is switched off entirely. Frames go out in the clear and anything can be injected. Bench use only. |

The passphrase only reaches the cipher at boot: rekeying a flying node would
strand it from every peer not yet changed. So set it, *Save*, and reboot. The
UI raises a *Reboot required* banner when a change needs one.

A stored passphrase reads back as dots. Posting the dots back means "leave it
alone", so you can edit other settings without retyping it.

## 4. Confirm peers are appearing

Power up two aircraft with the same passphrase and open the *Dashboard* on
either one. Each peer shows as a row with its UID, name, distance, bearing,
relative altitude, age and which radios it was heard on.

If the table stays empty, work through this in order:

1. *Radios*: at least one radio has to be enabled, and the same one on both
   ends. ESP-NOW is 2.4 GHz and short range; LoRa is long range and much
   slower per frame. A node can run both at once, and a peer heard on only
   one of them is worth noticing, which is why the peer row lists radios.
2. *Dashboard > crypto*: a climbing `bad_tag` count means frames are arriving
   and failing authentication, which is almost always a passphrase mismatch.
   Zero of everything means nothing is arriving at all: check radio, band and
   range.
3. *Settings > Node > Listen only*: a listen-only node tracks peers but never
   transmits, so the other aircraft will never see it.
4. Check both nodes actually rebooted after the passphrase change.

A peer with no GPS fix still appears in the table, because it is still
beaconing. It will not be followed: Follow ignores any peer whose beacons
carry no fix. See
[`user-guide-follow-mode.md`](user-guide-follow-mode.md).

## 5. Where to go next

- [`user-guide-follow-mode.md`](user-guide-follow-mode.md) sets up autonomous
  Follow Mode on an INAV aircraft.
- The *Simulator* page manufactures peers that fly real paths, so you can
  exercise the peer table, the radar output and Follow with one aircraft on a
  bench. See
  [`explainers/bench-testing-follow-mode.md`](explainers/bench-testing-follow-mode.md).
- [`v2-web-api.md`](v2-web-api.md) is the whole HTTP API, if you want to
  script configuration or poll status instead of clicking.
