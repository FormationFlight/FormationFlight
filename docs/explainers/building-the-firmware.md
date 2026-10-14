# Building FormationFlight After Code Changes

This explains how to turn local code changes into flashable firmware, both for a quick local check and for the way CI builds every target.

## How the build system fits together

FormationFlight is a [PlatformIO](https://platformio.org/) project (`platformio.ini` at the repo root). There is no separate "compile" step you write by hand - PlatformIO reads the `.ini` config, resolves the target environment, and drives the Arduino framework toolchain for you.

A few things happen automatically on every build, via `extra_scripts` in `platformio.ini`:

- `scripts/build_html.py` bundles everything in `html/` into a generated C header + web server handlers, so the device's web UI is compiled into the firmware.
- `scripts/build_flags.py` stamps the build with `VERSION`, `GITHASH`, and `BUILDTIME` derived from git, so the firmware can report exactly what it was built from.
- `scripts/env_setup.py` patches PlatformIO's OTA upload / compression behavior.

Targets are split across `targets/*.ini` (`unified.ini`, `diy_espnow.ini`, `diy_lora.ini`, `legacy.ini`), each defining PlatformIO environments named like `Unified_ESP8285_2400_via_UART` or `diy_ESPNOW_esp8266_via_UART`. The `Unified_*` ones run on ExpressLRS hardware and take their pins from the ExpressLRS targets repo at boot rather than at build time; see below. The `_via_UART` / `_via_WiFi` suffix picks the upload method; the rest of the name identifies the hardware/radio combination. You build one environment (one target) at a time.

There is also a host-native environment, `native`, which builds nothing for hardware: it compiles the pure logic in `lib/ff_core` and runs its unit tests on your machine. It deliberately overrides `extra_scripts` to empty, so none of the Arduino build steps above run for it.

## What the v2 build actually compiles

`build_src_filter` in `platformio.ini` restricts the firmware build to `src/main.cpp` and `src/hal/`. Everything under `src/lib/` is the v1 manager stack, kept in the tree as unbuilt reference until each family is ported, and excluded from every target. If you change a file under `src/lib/` and nothing happens, that is why.

Most of the actual logic is in `lib/ff_core/`, which is platform-independent and compiles for the host as well as for ESP32 and ESP8266.

## Building locally

### Option A: VS Code + PlatformIO (recommended for day-to-day dev)

1. Install [VS Code](https://code.visualstudio.com/) and the [PlatformIO IDE extension](https://platformio.org/install/ide?install=vscode).
2. Open the repo folder in VS Code - PlatformIO will detect `platformio.ini` automatically.
3. In the PlatformIO sidebar, pick the environment matching your hardware (e.g. `diy_ESPNOW_esp8266_via_UART`).
4. Click **Build** to compile, or **Upload** to compile and flash over the selected port.

PlatformIO manages all dependencies (Arduino cores, libraries, toolchains) itself - no manual toolchain setup is needed.

### Option B: PlatformIO CLI

Useful for scripting, CI-like local runs, or if you don't want the full IDE.

```bash
pip install platformio

# list available environments/targets
pio project config --json-output | grep -o '"env:[^"]*"'
# or simply: grep '^\[env:' targets/*.ini

# build one target
pio run -e diy_ESPNOW_esp8266_via_UART

# build and flash to a connected board
pio run -e diy_ESPNOW_esp8266_via_UART -t upload
```

Build output (the compiled firmware) lands in `.pio/build/<env-name>/firmware.bin`.

## Building for ExpressLRS (ELRS) receivers

ELRS receivers are built the way ExpressLRS itself builds them: one **unified** image per chip and band, with no pins compiled in, and the board described by its layout from the [ExpressLRS targets repo](https://github.com/ExpressLRS/targets). That repo is a submodule at `hardware/`, so fetch it once:

```bash
git submodule update --init hardware
```

The layout is appended to the image after the build, in the same 2.7 KB trailer ELRS uses (product name, device name, options, layout JSON). At boot the firmware reads it back from flash; a `/hardware.json` on LittleFS overrides it, as in ELRS. Because the trailer is ELRS' own, ELRS' tools for reading and patching it work on a FormationFlight device too.

### Which boards, which environment

```bash
python scripts/elrs_targets.py list          # supported receivers, their env and bands
python scripts/elrs_targets.py list --all    # everything, with why the rest are not
python scripts/elrs_targets.py show happymodel.rx_900.es900
```

That is every ESP8285, ESP32, ESP32-C3 and ESP32-S3 receiver with an SX127x, SX1280 or LR1121 and a UART to talk MSP over, with or without a PA/LNA, a second radio or an antenna switch. What is left out: PWM-only receivers (no UART), boards with an on-board VTx (it would come up wherever its defaults put it), a PA set by DAC, and the STM32 boards.

There is one environment per chip and MCU, named after the ELRS firmware it stands in for without the `_RX`. The band is not part of the build:

| Environment (`_via_UART` / `_via_WiFi`) | Chip | Bands the board can run |
|---|---|---|
| `Unified_ESP8285_900`, `Unified_ESP32_900`, `Unified_ESP32C3_900` | SX127x | 868, 915 |
| `Unified_ESP8285_2400`, `Unified_ESP32_2400`, `Unified_ESP32C3_2400`, `Unified_ESP32S3_2400` | SX1280 | 2400 |
| `Unified_ESP32_LR1121`, `Unified_ESP32C3_LR1121`, `Unified_ESP32S3_LR1121` | LR1121 | 868, 915, 2400 |

### The band is a setting

Which band LoRa runs on is `radios.lora_band` (Settings, Radios, in the web UI), and changing it takes a reboot. The channel, modulation, duty-cycle ceiling and power ceiling for each band are in `lib/ff_core/lora_band.cpp`, with the reasoning behind each number.

ExpressLRS hardware is never offered 433: the chips would tune it, but the RF front ends are matched for 868/915 or 2.4 GHz. 433 is only available on the hand-written targets in `targets/*.ini` built for it.

The band a freshly flashed board boots on is stored in the trailer by `--band` (below). An SX1280 board needs none; it only has one. A 900 or LR1121 board flashed without one boots with LoRa **off**, logs why, and the web UI asks for a band, rather than the firmware guessing one that is illegal in half the world.

### How the hardware is driven

All of it is in `src/hal/RadioLoRa`, one driver for all three chips:

- **PA/LNA switched by the ESP** (`power_rxen`/`power_txen`, and `_2` for a second radio): rx-enable high while listening, every LNA off and the one PA on while transmitting. `power_enable` is held high from boot.
- **LR1121**: no ESP involvement per frame. The layout's `radio_rfsw_ctrl` (or ELRS' default) is handed to the chip as its `SetDioAsRfSwitch` table and the chip switches its own front end. `radio_rfo_hf` selects the low-power sub-GHz PA, `radio_tcxo` the TCXO supply, `radio_dcdc` the regulator.
- **Two radios on one band** (`radio_nss_2`): both receive; the stronger copy of a frame is kept and the other counted as a duplicate; frames go out on whichever radio has been hearing best. Two radios on *different* bands (a dual-band "Gemini" board run as such) is the next step; for now both run on `radios.lora_band`.
- **Antenna switch** (`ant_ctrl`, one radio): stays on the better antenna, flipping between frames when the other has been doing better, and drifts back to try the idle one now and then.
- **Power**: `radios.lora_power_dbm` (the LoRa TX power setting in the web UI) picks the highest ELRS power level at or below it (10 mW for 0, never below the board's lowest level), capped by the band's power ceiling (`lora_band.cpp`), and applies live. The dashboard reports the level's nominal output, which on a PA board is a long way from what the radio itself is set to. RSSI is reported with the LNA gain taken off, so boards with and without an LNA are comparable.

### Building one

Name the board with its dotted `targets.json` path, and the band to boot on, and the build appends both:

```bash
ELRS_TARGET=happymodel.rx_900.es900 ELRS_BAND=915 pio run -e Unified_ESP8285_900_via_UART -t upload
```

(or `custom_elrs_target` and `custom_lora_band` in an `[env:...]` section). The build refuses a board that the environment does not match, a band the board's hardware is not matched for, or a board that is not supported. To configure an image that is already built, such as one from CI:

```bash
python scripts/elrs_targets.py configure happymodel.rx_900.es900 Unified_ESP8285_900.bin --band 915 -o es900.bin
```

Configuring is idempotent: it replaces any trailer already there.

Built with no target, a unified image still boots, but with no radio and no MSP, and it logs an error saying so; `node.layout` in `/api/status` reads `none`.

## Running the tests

Everything off-hardware runs from one script:

```bash
./scripts/run_tests.sh
```

That is the native Unity suites (`pio test -e native`), the web UI's `follow-logic.js` tests (`node --test test/follow-logic.test.js`), and the mock server's API and config-validation tests (`python3 test/test_mock_server.py`), which is the same set `.github/workflows/test.yml` runs. Run them individually if you only touched one area. `pio test -e native` is the one that covers the protocol, crypto, peer table, rate control, geodesy, Follow and the traffic simulator.

For UI work, `python3 scripts/mock_server.py` serves `html/` against an in-memory node implementing every endpoint in [`../v2-web-api.md`](../v2-web-api.md), so the web UI can be developed with no hardware attached.

## After your changes, before opening a PR

1. Build at least one representative target locally (`pio run -e <target>`) to confirm the change compiles.
2. Run `./scripts/run_tests.sh`. CI runs the same suites, so a failure there is a failure here.
3. If you touched anything under `html/`, rebuild so `scripts/build_html.py` regenerates the embedded web UI - this runs automatically as part of `pio run`, but a stale `.pio/` cache can hide breakage, so a clean build (`pio run -e <target> -t clean && pio run -e <target>`) is worth doing if you're unsure.
4. Push/open a PR. GitHub Actions (`.github/workflows/build.yml`) builds every `_UART`/`_via_UART` environment across all `targets/*.ini` files automatically and uploads the resulting `.bin` files as workflow artifacts, so you can sanity-check the full matrix without building every target yourself.

## What CI does differently

The `build.yml` workflow:

- Dynamically discovers targets by grepping `targets/*.ini` for `[env:...UART]` entries (excluding anything marked `DEPRECATED`).
- Installs PlatformIO fresh and caches `~/.platformio` and pip between runs.
- Runs `pio run -e <target>` per target as a matrix job.
- Collects each target's `release/` folder (written by `package_release()` in `scripts/env_setup.py`): `<target>.bin`, `<target>.flash.json`, and for ESP32-family targets its own `<target>.bootloader.bin`/`<target>.partitions.bin` plus `boot_app0.bin`; and `<target>.bin.gz` for ESP8266 targets, the file their Update page needs.
- Fails an ESP8266 target whose `.bin.gz` no longer fits the space an over-the-air update is received into. On a 1 MB ESP8285 that space is under half the flash, and PlatformIO's "Flash: N% used" figure does not show it. If this trips, shrink the image or the filesystem in `targets/esp8285_1m.ld`.
- Builds `index.json` for the web installer (`scripts/flasher_manifest.py`) from all of them, and on master publishes the lot as the rolling `development` prerelease.
- On a `v*` tag push, packages all artifacts into a GitHub Release automatically.

You don't need to replicate this locally - it's mainly useful context if a CI build fails for a target you didn't test.
