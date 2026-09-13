# v2 hardware bring-up

No part of v2 has run on hardware. Everything in it is compile-verified and
covered by host tests, which catches logic errors and catches nothing about
timing, SPI, interrupts, RF, or the ESP8266's temper. This is the order to find
that out in, arranged so each step only depends on things the previous steps
already proved.

Have a serial console open throughout. The MSP UART is the same port on ESP8266
targets, so console output and MSP share it; that is expected.

## 0. Before the first flash

Back up whatever is on the board. v2 does not interoperate with v1 at all: the
wire format, the crypto and the config storage are all different, so a v2 node
and a v1 node in the same airspace cannot see each other and neither is at
fault.

`pio run -e <target> -t upload`. The first boot writes `/config.json` with
defaults; nothing is read from EEPROM ever again.

## 1. It boots and serves

- Join the WiFi AP `FormationFlight-<uid>`, open `http://192.168.4.1/`.
- The Dashboard should show the node's UID, an uptime that climbs, and free
  heap. On an ESP8285 expect somewhere around 20-25 KB free with the web server
  running. If free heap is under about 8 KB, stop and investigate: that is the
  region where the async web server starts failing allocations and behaving
  strangely rather than failing cleanly.
- Check `Settings` loads and shows defaults.

**If the AP does not appear:** the serial log is the only source of truth.
Expect the ESP8266 to reboot-loop if `LittleFS` has never been formatted.

## 2. Configuration survives a power cycle

- Set the node name, press **Apply**, confirm it changes in the header.
- Power cycle without saving. The name must come back to its old value: Apply is
  RAM-only and that is the property that makes a bad setting recoverable.
- Set it again, press **Save**, power cycle. Now it must persist.
- `curl http://192.168.4.1/api/config` and confirm the secrets read `••••••••`
  rather than the real values.

## 3. One radio, one node

With a single node powered, on the **Radios** page:

- Every enabled radio should show `tx` climbing.
- `rx_self` climbing on ESP-NOW is normal and expected: ESP-NOW broadcast is
  heard by the sender. It is counted separately precisely so it does not look
  like a peer.
- `beacon_interval_ms` should sit at the 100 ms floor with no peers.
- `rx_dropped` and `tx_dropped` must stay at zero. Either one climbing on an
  idle single node means something is wrong before any RF is involved.

## 4. The simulator, before any second board

This is the cheapest way to exercise the whole receive path, and it runs the
real decode, the real AES-CCM authentication and the real replay check.

- **Settings** or the **Simulator** page: turn the simulator on.
- Add a peer: mode `hex`, radius 100 m, speed 15 m/s, centred on your own
  position (or anything, if you have no fix yet).
- The Dashboard peer table should show it within a second or two, moving.
- The frame log should show `ok` frames arriving on the `SIM` radio.
- Turn the simulator off. The peer must age out and disappear.

A simulated peer that never appears means the receive path is broken
independently of RF, which is a much easier fault to chase than an RF one.

## 4b. Check the channel you are actually on

The LoRa channel defaults were repicked; the old ones were wrong in ways worth
knowing about if you have older notes or hardware programmed from them.

| Band | Centre | BW | SF | CR | Airtime | Sensitivity | Duty floor |
|---|---|---|---|---|---|---|---|
| 433 | 434.400 MHz | 250 kHz | 7 | 4/5 | 36 ms | -121.5 dBm | 360 ms |
| 868 | 869.525 MHz | 125 kHz | 7 | 4/5 | 72 ms | -124.5 dBm | 719 ms |
| 915 | 920.000 MHz | 500 kHz | 8 | 4/7 | 43 ms | -121.0 dBm | none |
| 2400 | 2476.000 MHz | 812.5 kHz | 8 | 4/7 | 27 ms | -118.4 dBm | none |

Sensitivity is the calculated demodulation floor, `-174 + 10log10(BW) + NF +
SNR_limit`, with a 6 dB noise figure (6.5 at 2.4 GHz). It is a model, not a
measurement, and worth checking against a real receiver once there is one.

Spreading factor is picked per band against its duty-cycle ceiling rather than
uniformly. Where there is no ceiling the airtime is cheap and worth spending on
range; where there is one, every extra millisecond directly slows the beacon,
and those bands are already the most sensitive because their channels are
narrower.

Three consequences to expect on the bench rather than be surprised by:

- **2.4 GHz used to be the weak link by a factor of six.** At SF5 it managed
  about 2.7 km of free-space range against 17 to 52 km for the sub-GHz bands:
  8.6 dB of that is just the extra path loss over 915 MHz, and the rest was
  SF5 being 7.5 dB less sensitive than SF8. It is now in the same sensitivity
  class as the others. Expect the LoRa beacon to be noticeably slower than it
  was, which is the trade: ESP-NOW carries the fast updates when you are close,
  and LoRa is the layer that should still be there when you are not.

- **EU 868 cannot beacon faster than about 1.4 Hz.** The 869.40-869.65 sub-band
  permits 10% duty, and 72 ms of airtime per frame is what that buys. The rate
  controller enforces it as a floor, so setting `rate.min_interval_ms` to 100
  will not make it go faster, and that is deliberate. LoRa is the long-range
  medium; ESP-NOW carries the fast updates.
- **2.4 GHz LoRa moved to 2476 MHz, well away from ESP-NOW.** The old default
  sat on top of WiFi channel 1, a couple of centimetres from its own antenna.
  If you change `wifi.channel` to 11 or 13, check for interference again,
  because you will have moved WiFi back toward the LoRa channel.

A build will now refuse to compile if a channel falls outside its band, so you
cannot reintroduce the old values by accident.

## 5. Two nodes, one medium

Flash a second board. **Set the same group passphrase on both**, or they will
not see each other: the frames authenticate against a key derived from it, and a
mismatch is indistinguishable from noise by design.

- Each node's peer table should list the other, with a plausible RSSI.
- `rx_ok` climbs on both.
- Watch `rx_crypto_fail`. A steady climb with no peers appearing is the
  signature of mismatched passphrases. Occasional ones in a noisy band are
  normal, because a corrupted frame fails the tag rather than a CRC.
- Pull the power on one. The other should expire it after `peers.timeout_ms`.
- Power it back up. It must rejoin. If it does *not* rejoin for about ten
  seconds, that is the replay resync window doing its job, not a bug: the
  rebooted node restarted its frame counter, and the receiver will not accept a
  counter going backwards until the sender has been quiet long enough to have
  plausibly rebooted.

## 6. Two nodes, both media at once

The part with the least prior art, and the reason most of this document exists.

**Check the channel first.** On the Radios page, `wifi.channel` must be the same
number on both nodes. ESP-NOW rides whatever channel the WiFi radio is on, so
two nodes on different channels cannot hear each other over ESP-NOW while both
report themselves entirely healthy. If either node has joined an external
network, the router owns its channel and this is the most likely thing to be
wrong.

With both ESP-NOW and LoRa enabled on both nodes:

- Each peer row should show *both* radios in its "heard on" chips. One solid and
  one dashed means that medium is not delivering, and the dashed one names it.
- Both radios should have `rx_ok` climbing independently.
- `beacon_interval_ms` should differ between them: ESP-NOW sits near the 100 ms
  floor while LoRa backs off, because each radio's rate is sized from its own
  airtime and its own peer count. Identical intervals on both mean the per-radio
  pacing is not working.
- `rx_replay` should stay at or near zero. **If it climbs in step with LoRa
  receives, that is the cross-medium reordering case**: the two media have very
  different latency, so a LoRa frame sent first routinely arrives after an
  ESP-NOW frame sent later and carrying a higher counter. The receiver keeps a
  32-deep sliding window specifically to accept that. A climbing replay count
  means the window is not doing its job and LoRa frames are being discarded.
- `tx_dropped` on LoRa should stay at zero. It counts transmits refused because
  the radio was still busy with the previous frame, which can happen when a
  beacon and an announce fall in the same loop iteration. A few are survivable,
  ALOHA is built for it. A steady climb means the beacon interval is too short
  for the airtime, or transmit-done interrupts are being missed.

Then disable one radio at a time from Settings and confirm the other keeps
working on its own.

## 7. Flight controller

- Connect the MSP UART. The Dashboard's `fc` block should fill in: variant,
  version, and the platform type once INAV answers `MSP2_INAV_MIXER`.
- `armed` should follow the FC's arm state. v2 reads INAV's arming flags rather
  than the ARM box, so it reports *armed*, not *the switch is up*.
- Confirm your own position appears once the FC has a fix, and that peers show
  distance and bearing (they are omitted entirely until you have a fix of your
  own, because there is nothing to measure from).
- Check the FC's OSD radar shows the peers. That path runs on its own timer and
  is deliberately independent of transmit activity.

**Unverified and worth watching:** MSP2_INAV_STATUS is what v2 uses for mode
flags on INAV, because MSP_STATUS's 32-bit field cannot reach GCS NAV on a build
with more than 32 boxes enabled. That reasoning is from the INAV source, not
from a bench. If `gcs_nav` never goes true with the mode active, this is the
first thing to look at.

## 8. Follow

Only after everything above. Use the simulator as the leader first, with the
aircraft on the bench and props off.

- Engage GCS NAV. Follow's state should go ACQUIRING then LOCKED.
- Confirm the target position tracks the simulated leader.
- Check the altitude floor clamps rather than suppressing the waypoint.
- Then, and only then, with a real second aircraft.

## 9. OTA

Do this last, and over a wired-quality link.

- **Update** page, upload a `.bin` (`.bin.gz` also works on ESP8266).
- The radios deliberately stop while an upload is in flight: flash writes stall
  the CPU for milliseconds at a time, and beaconing through that produces
  corrupt transmissions and a starved web server.
- A failed upload (try a wrong file deliberately) must leave the node running
  and reachable, radios back on, not wedged. That path is easy to get wrong and
  worth testing on purpose.

## Things that would not surprise me

Written down so they are recognised rather than debugged from scratch:

- **ESP8266 watchdog resets under load.** Three radios, a web client polling and
  an MSP link is a lot for an 80 MHz core. The frame log and the `free_heap`
  reading are the first places to look.
- **SPI contention on shared-bus targets.** The LoRa driver takes the bus in
  `serviceRx()` from the main loop, never from the ISR, which should be safe,
  but it has not been proved on hardware.
- **RSSI reads of 0 on ESP-NOW.** Expected: the callback gives no per-packet
  signal level. It is a sentinel, not a measurement.
- **LoRa airtime not matching the model.** `airtime_ms` is computed from the
  modulation parameters, not measured. If the observed beacon rate disagrees
  with the displayed interval, the airtime model is the suspect.
