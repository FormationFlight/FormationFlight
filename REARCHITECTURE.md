# FormationFlight v2 Rearchitecture Plan

Decisions locked in:

- **Platforms**: ESP32-family *and* ESP8266/8285 both stay first-class. No FreeRTOS
  dependency in application code (ESP8266_RTOS_SDK is incompatible with the Arduino
  ecosystem we depend on: RadioLib, ESPAsyncWebServer, ArduinoOTA).
- **Protocol**: clean break. v2 does not interoperate with v1. Version byte reserves
  room for future evolution.
- **Channel access**: randomized (ALOHA-style) beaconing. No slots, no sync, no master,
  no slot-ID coordination — the entire TDMA machine is deleted.
- **Rate**: adaptive. Transmit interval scales with the number of active peers to hold
  channel utilization at a target; nodes converge on the same rate automatically.

---

## Why this shape

The v1 design needed millisecond-accurate slot timing from a cooperative Arduino loop
full of blocking MSP serial reads, `delay()` calls, and OLED redraws — and the slot
coordination itself was buggy (`pick_id()` overwrites its own collision avoidance;
sync is chained off whichever peer sits in slot 1 as an accidental, unelected master;
peer timestamps are stamped at loop-processing time, not receive time).

ALOHA removes the precision requirement instead of trying to satisfy it. Deliberate
random jitter on every transmission means loop-level jitter is harmless, which is what
makes a single portable execution model viable on both ESP32 and ESP8266.

Collision math sanity check: at SF6/250kHz a ~20-byte packet is roughly 5–8 ms of
airtime. Channel load G = N × rate × airtime. Adaptive rate targets G ≈ 0.15, where
per-packet collision probability (pure ALOHA, ≈ 1−e^(−2G)) stays ~25%, and LoRa's
capture effect (stronger packet decodes anyway) improves on that in practice. Position
beacons are redundant by nature — a 25% loss rate at 4–10 Hz still yields smooth peer
tracking. SX127x/SX128x CAD (channel activity detection) can be layered on later as a
cheap listen-before-talk optimization; it is not required for correctness.

---

## Phase 0 — Safety net (before touching behavior)

- Add a PlatformIO `native` environment. Extract protocol encode/decode, peer table,
  rate control, and crypto into pure modules with no Arduino/hardware includes.
- Unit tests with a simulated clock and simulated radio: collision behavior, rate
  convergence, peer timeout/expiry, packet round-trips, replay rejection.
- CI: tests + all target builds must pass (build workflow already exists).

## Phase 1 — Execution model

Replace the megaloop + globals with an event-driven core, identical on both platforms:

- **Event loop + timer scheduler** (pure C++, natively testable). Modules register
  timer callbacks and event handlers; nothing blocks, ever.
- **Radio RX**: ISR captures a timestamp and pushes raw frames into a small ring
  buffer; the event loop drains it. Timestamps are receive-time, not process-time.
- **Radio TX**: scheduled via hardware timer (esp_timer on ESP32, timer1/os_timer on
  8266). The event loop only *decides* when the next beacon goes out.
- **MSP**: rewritten as a byte-pumped non-blocking state machine (the current
  implementation busy-waits up to a timeout per request, inside the main loop).
  Request/response becomes async with callbacks or polled completion.
- **State**: `sys`/`curr`/`cfg` globals and the `MODE_*` phase machine die. Each
  module owns its state behind an interface; cross-module access goes through
  explicit references wired once at startup (no lazy singletons). The web server
  (async task on ESP32) reads snapshot copies, not live structs.
- Fix known driver bugs while in there: SX128X IRQ flag exact-equality dropping
  packets, `packetReceived` ISR race, inconsistent `last_tx_end` semantics.

## Phase 2 — Protocol v2 (ALOHA)

Identity is a **32-bit UID** (from efuse MAC), not a slot number. Two packet types:

- **Position beacon** (frequent): version/type, UID, lat, lon, alt, ground speed and
  course *every time* (no more round-robin field multiplexing), state flags.
- **Announce** (every ~2 s): UID, full craft name, firmware version, capabilities.
  Kills the 1-byte-of-name-per-packet dribble.

**Rate control**: each node counts active peers N (heard within timeout). Beacon
interval = N × airtime / G_target, clamped to [100 ms, 1000 ms], plus uniform
±25% jitter. Every node computes the same thing from the same observation, so rates
converge without coordination. Airtime is computed per radio mode at init.

**Crypto**: XTS-AES with a static tweak (and its 16-byte-exactly packet constraint)
is replaced by an AEAD with a truncated MIC — AES-CCM with 4–8 byte tag, nonce =
UID + monotonic counter (counter travels in the clear in the header). Gives
integrity + replay rejection; CRC8 becomes redundant and is dropped. Tag length is
an airtime trade-off to finalize against measured airtime budgets.

**Peer table**: keyed by UID, capacity a compile-time constant (larger than 6),
LRU eviction of expired peers. MSP radar output maps the nearest/strongest peers
onto the FC's radar slot limit.

## Phase 3 — Config, UI, polish

- One ConfigManager (the current `config_init` contains `if (true || ...)` — EEPROM
  persistence has been dead code; `ConfigManager` is an empty stub of an abandoned
  migration). Versioned struct, actually persisted, editable via web UI: group key,
  craft name, radio band/power, G_target, display options.
- Web UI updated for UID-based peers and new status/telemetry (channel load, TX
  rate, per-peer loss estimates).
- Display pages updated accordingly; scan/sync phases disappear from UX (node is
  live within one beacon interval of boot).

## Deletion inventory (the satisfying part)

- `pick_id()`, `resync_tx_slot()`, drift correction, `MODE_OTA_SCAN`/`MODE_OTA_SYNC`
  phases, slot-conflict handling, `sys.next_tx` polling, silent mode (replaced by a
  simple listen-only config flag for GCS), byte-dribbled names, CRC8, XTS crypto,
  dead EEPROM path, `ConfigHandler`/`ConfigManager` duplication, lazy singletons.

## Sequencing

Each phase lands as its own PR(s) on a `v2` branch, buildable at every step.
(Superseded refinement: because we took the clean break up front, Phase 1 wires
the v2 modules directly rather than temporarily porting the v1 packet.)

## Status (living)

Done and committed on `v2`:

- **Phase 0** — native test harness; pure modules: wire protocol, LoRa airtime,
  ALOHA rate control, UID peer table.
- **Phase 1a** — pure execution-model primitives: cooperative timer scheduler,
  lock-free SPSC ring.
- **Phase 1b** — `ff::Node` application core: the full v2 state machine behind
  interfaces (IRadio/ILocationSource/ICrypto/RNG); replaces the MODE_* phase
  machine and the sys/curr/cfg globals. Host-tested with fakes.
- **Phase 1c** — hardware adapter layer + new `main.cpp`; first flashable build
  (`expresslrs_rx_2400_via_UART`, ESP8285). `ff_core` compiles/links for host,
  ESP32, and ESP8266.
- **Phase 1d** — multi-radio. The Node addresses radios by index via an IRadioSet
  (implemented by `RadioHub`) and beacons on each **independently**: every radio
  has its own beacon timer paced from its own airtime, so ESP-NOW stays fast
  (tiny airtime → clamps to the 10 Hz floor) while LoRa backs off as peers grow.
  Announces fan out to all radios; frames received on any radio feed the one peer
  table. Adapters for all three families exist (ESP-NOW, SX127x, SX128x) and a
  node runs ESP-NOW + LoRa *simultaneously* — short-range 2.4 GHz mesh bridged to
  long-range LoRa, which slot-based v1 could never do. Builds verified on ESP8266
  (SX128x+ESP-NOW) and ESP32 (SX127x+ESP-NOW).
- **Phase 1e** — per-medium peer counting + non-blocking MSP. Each peer records a
  per-radio last-heard time, so a radio's rate is driven by the peers actually on
  *that* medium (LoRa-only peers no longer slow ESP-NOW). MSP reads are now a pure
  byte-fed `MspParser` (host-tested) drained from `MspLocationSource::service()`;
  the last blocking serial wait in the firmware is gone.
- **Phase 1f** — auto-configuring direct GPS. A pure, host-tested UBX module
  (`ubx.*`: NAV-PVT parser/decoder + CFG-RATE/PRT/MSG builders, checked against
  the canonical u-blox 10 Hz command). `DirectGpsLocationSource` (hal) runs a
  non-blocking state machine that baud-sweeps an attached u-blox module, raises it
  to 115200, sets the navigation rate high (10 Hz default, `GNSS_RATE_HZ`), and
  switches it to compact binary NAV-PVT with NMEA off — replacing v1's 1 Hz NMEA.
  Builds on the T-Beam target.

- **Phase 1g** — fixed a real, previously-observed v1 bug: a listen-only (GCS)
  node's received peers never reached the USB-attached ground station, because
  v1 only pushed MSP output as a side effect of the node's own radio transmit
  cycle -- which a listen-only node, by definition, never runs. Added the MSP
  radar-output path v2 was missing entirely (`ff_core/msp_radar`: a pure,
  host-tested `MSP2_COMMON_SET_RADAR_POS` builder; `hal/MspRadarOutput`, which
  writes and explicitly flushes). Wired via `Node::IMspRadarSink` on its **own**
  independent scheduler timer -- never gated by `listen_only`, radio count, or
  transmit activity -- so this bug class is structurally impossible to
  reintroduce. A `GCS_MODE` build flag sets `listen_only` until real runtime
  config (Phase 3) exists. The MSP UART is now always brought up regardless of
  which location source is active (previously unwired for GNSS_ENABLED targets).

- **Phase 1h** — Follow-on-iNav (PR #71, adhishvyas) merged and ported onto the
  Node core. The v1 contribution steers an iNav FC into a formation slot
  relative to another node (WP#255 + MSP_SET_HEAD, RC-scaled offsets with a
  two-layer geometry safety net, altitude floor, GVAR status/condition
  reporting, fixed-wing autothrottle). It now lives in `ff_core/follow.*` as a
  pure, host-tested `FollowController` that reads the Node's UID-keyed
  `PeerTable` and `ILocationSource` and talks to the FC through an `IFollowFc`
  seam. Targets are 32-bit UIDs (0 = first active); the v1 "slot id reused by
  another aircraft" hold-state hack is gone because a UID is a stable identity.
  Geodesy was extracted from v1's GNSSManager into pure `ff_core/geo.*`.
  The FC side is `hal/MspFcLink`, which replaces `MspLocationSource`: one
  non-blocking MSP link (v1 *and* v2 framing in `MspParser`, protocol helpers
  in `ff_core/msp_fc.*`) that polls the fix, modes and Follow's telemetry on
  schedules, never waits, and sets the beacon's ARMED flag from the FC. Modes
  come from MSP2_INAV_STATUS's full-width box bitmask on INAV (MSP_STATUS's
  32-bit field can't see GCS NAV on builds with many boxes enabled), and
  arming from INAV's armingFlags rather than the ARM box. `hal/FollowConfigStore`
  persists the versioned `FollowRecord` in EEPROM (offset 0 -- Phase 3's
  ConfigManager must place itself after it or absorb it). The PR's web panel,
  mock server, docs and fixture came across unchanged apart from dropping the
  v1 slot-id range rule from all three config validators; the web UI itself
  still returns in Phase 3. The v1 `src/lib/Follow` copy stays as unbuilt
  reference for that port (its REST handlers).

- **Phase 2** — protocol crypto. AES-128-CCM replaces v1's XTS-AES, which
  authenticated nothing and forced a fixed 16-byte packet. The frame carries
  version, type, UID and a counter in the clear as associated data, then the
  ciphertext, then a 6-byte tag: 10 bytes over the plaintext packet, and the
  rate controller now sizes airtime from the frame that actually flies. The UID
  stays readable because the receiver needs it to derive the nonce; everything
  else is encrypted. Replay is rejected by a per-sender monotonic counter with a
  resync window so a rebooted aircraft rejoins, and the residual exposure that
  window leaves is documented in `crypto.h` rather than hidden. AES and SHA-256
  are written out in `ff_core` instead of pulled from a platform library, so the
  cipher and the passphrase-to-key derivation are bit-identical on ESP32,
  ESP8266 and the host, and testable against FIPS-197, the NIST SHA examples and
  RFC 3610 (all cross-checked against OpenSSL before being committed).

- **Phase 3** — configuration, web API and a bench simulator.
  - **Config** is one JSON document in LittleFS (`ff_core/config.h` +
    `hal/ConfigStore`), replacing every EEPROM struct including Follow's own
    record. Partial updates merge rather than replace, an unparseable file is
    preserved rather than overwritten, and secrets are redacted on the way out.
  - **Web API** (`hal/WebServer`, contract in `docs/v2-web-api.md`): status,
    config, the frame log, the simulator, reboot and firmware upload. RAM-first
    config changes with an explicit save, so a bad setting is a power cycle away
    from recovery. No websocket, deliberately: heap on the 8285 is the binding
    constraint and the UI polls.
  - **Observability**: per-radio counters and a 32-entry frame log on the Node,
    which is what makes "one medium deaf while the other works" visible instead
    of being buried in an aggregate.
  - **Simulator** (`ff_core/sim_traffic` + `hal/SimRadio`): peer motion in
    closed form, encoded into real packets, encrypted with the real group key,
    and pushed through `Node::onReceive`. The HITL path exercises the receive
    path rather than bypassing it.

Test status: 266 host unit tests green (v2 core + the ported Follow suite +
geodesy + MSP protocol). `ff_core` proven on xtensa-lx106 and xtensa-esp32 via
real firmware links.

Remaining / deferred:

- **T-Beam GPS power** — the onboard GPS is powered by the AXP192 PMIC; enabling
  that rail (v1's TBeamPower) is not yet ported, so the direct-GPS driver runs but
  the T-Beam module stays unpowered until that follow-up lands. Any externally
  powered GPS UART works today.
- **On-device validation** — builds are compile-verified only; no hardware bring-up
  has been done yet.
- **Legacy code** — the v1 managers remain in `src/lib` (unbuilt) as reference for
  porting; they get deleted once each family is ported. `src/lib/Follow` is
  already ported (Phase 1h), and its REST handlers have been superseded by the
  v2 web API, so it can be deleted outright.
- **Display** — the v1 OLED stack is still excluded from the v2 build. It is
  the last subsystem that has not been ported onto Node snapshots.

Next: the OLED display is the last v1 subsystem still unported, and no part of
v2 has been validated on hardware yet -- everything above is compile- and
host-verified only.
