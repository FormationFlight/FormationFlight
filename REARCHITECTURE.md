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
- **Phase 1d** — multi-radio. A `RadioHub` (an IRadio composite) fans each beacon
  out to every enabled radio, reports the max child airtime so ALOHA paces to the
  slowest medium, and drains all radios into the one peer table. Adapters for all
  three families now exist (ESP-NOW, SX127x, SX128x) and a node runs ESP-NOW +
  LoRa *simultaneously* — short-range 2.4 GHz mesh bridged to long-range LoRa,
  which slot-based v1 could never do. Builds verified on ESP8266 (SX128x+ESP-NOW)
  and ESP32 (SX127x+ESP-NOW).

Test status: 58 host unit tests green. `ff_core` proven on xtensa-lx106 and
xtensa-esp32 via real firmware links.

Remaining / deferred:

- **Async MSP** — `MspLocationSource` is rate-limited + cached (beacon path never
  blocks), but the MSP request itself is still bounded-blocking. A fully
  non-blocking MSP byte-pump is a later refinement.
- **Web / OTA** — the v1 WiFi/web/OTA stack is excluded from the v2 build for now;
  it returns in Phase 3 reading Node snapshots.
- **On-device validation** — builds are compile-verified only; no hardware bring-up
  has been done yet.
- **Legacy code** — the v1 managers remain in `src/lib` (unbuilt) as reference for
  porting; they get deleted once each family is ported.

Next phases: **2** — protocol v2 crypto (AES-CCM AEAD replacing the passthrough)
and finalizing the wire format; **3** — config persistence, web UI, display.
