# Building FormationFlight After Code Changes

This explains how to turn local code changes into flashable firmware, both for a quick local check and for the way CI builds every target.

## How the build system fits together

FormationFlight is a [PlatformIO](https://platformio.org/) project (`platformio.ini` at the repo root). There is no separate "compile" step you write by hand — PlatformIO reads the `.ini` config, resolves the target environment, and drives the Arduino framework toolchain for you.

A few things happen automatically on every build, via `extra_scripts` in `platformio.ini`:

- `scripts/build_html.py` bundles everything in `html/` into a generated C header + web server handlers, so the device's web UI is compiled into the firmware.
- `scripts/build_flags.py` stamps the build with `VERSION`, `GITHASH`, and `BUILDTIME` derived from git, so the firmware can report exactly what it was built from.
- `scripts/env_setup.py` patches PlatformIO's OTA upload / compression behavior.

Targets are split across `targets/*.ini` (`diy_espnow.ini`, `diy_lora.ini`, `legacy.ini`, `expresslrs.ini`), each defining PlatformIO environments named like `diy_ESPNOW_esp8266_via_UART` or `expresslrs_rx_2400_via_WiFi`. The `_via_UART` / `_via_WiFi` suffix picks the upload method; the rest of the name identifies the hardware/radio combination. You build one environment (one target) at a time.

## Building locally

### Option A: VS Code + PlatformIO (recommended for day-to-day dev)

1. Install [VS Code](https://code.visualstudio.com/) and the [PlatformIO IDE extension](https://platformio.org/install/ide?install=vscode).
2. Open the repo folder in VS Code — PlatformIO will detect `platformio.ini` automatically.
3. In the PlatformIO sidebar, pick the environment matching your hardware (e.g. `diy_ESPNOW_esp8266_via_UART`).
4. Click **Build** to compile, or **Upload** to compile and flash over the selected port.

PlatformIO manages all dependencies (Arduino cores, libraries, toolchains) itself — no manual toolchain setup is needed.

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

ELRS receiver targets live in `targets/expresslrs.ini` and are built exactly like any other target — pick the environment matching your receiver's band/hardware and run PlatformIO against it. There is no separate ELRS-specific tooling; the differences between receivers are just build flags (pin mapping, RF band, PA/LNA, antenna diversity, RGB LED) baked into each environment.

Available `expresslrs_rx_*` environments (all with `_via_UART` and `_via_WiFi` upload variants):

| Environment (`_via_UART` / `_via_WiFi`) | Band | Example hardware |
|---|---|---|
| `expresslrs_rx_868` | 868MHz | HappyModel ES900RX |
| `expresslrs_rx_915` | 915MHz | HappyModel ES900RX |
| `expresslrs_rx_2400` | 2.4GHz | HappyModel EP2 |
| `expresslrs_rx_2400_PA` | 2.4GHz, with PA/LNA | BetaFPV Nano |
| `expresslrs_rx_2400_PA_RGB` | 2.4GHz, with PA/LNA + RGB LED | Foxeer ELRS 2.4GHz LNA Receiver |
| `expresslrs_rx_2400_AntennaDiversity` | 2.4GHz, with antenna diversity | Matek R24-D |

To build one, e.g. for a HappyModel EP2 flashed over its serial/UART bootloader:

```bash
pio run -e expresslrs_rx_2400_via_UART
```

Or to build for OTA/WiFi upload instead (useful for re-flashing a receiver that's already running FormationFlight or ELRS's WiFi update mode):

```bash
pio run -e expresslrs_rx_2400_via_WiFi
```

Swap in the environment name for your specific band/hardware from the table above. As with any target, the compiled binary ends up at `.pio/build/<env-name>/firmware.bin`. In VS Code, the same environments appear in the PlatformIO sidebar and can be built/uploaded with a click.

All ELRS RX targets build on the ESP8266 (`env_common_esp82xx`) base and share LoRa pin config for their RF module family — see `targets/expresslrs.ini` for the exact pin/flag definitions if you're adding support for new ELRS hardware.

## After your changes, before opening a PR

1. Build at least one representative target locally (`pio run -e <target>`) to confirm the change compiles.
2. If you touched anything under `html/`, rebuild so `scripts/build_html.py` regenerates the embedded web UI — this runs automatically as part of `pio run`, but a stale `.pio/` cache can hide breakage, so a clean build (`pio run -e <target> -t clean && pio run -e <target>`) is worth doing if you're unsure.
3. Push/open a PR. GitHub Actions (`.github/workflows/build.yml`) builds every `_UART`/`_via_UART` environment across all `targets/*.ini` files automatically and uploads the resulting `.bin` files as workflow artifacts, so you can sanity-check the full matrix without building every target yourself.

## What CI does differently

The `build.yml` workflow:

- Dynamically discovers targets by grepping `targets/*.ini` for `[env:...UART]` entries (excluding anything marked `DEPRECATED`).
- Installs PlatformIO fresh and caches `~/.platformio` and pip between runs.
- Runs `pio run -e <target>` per target as a matrix job.
- Collects `firmware.bin` (and `.bin.gz` if present) per target, plus `bootloader_dio_40m.bin`/`partitions.bin`/`boot_app0.bin` for ESP32 targets, into `~/artifacts/`.
- On a `v*` tag push, packages all artifacts into a GitHub Release automatically.

You don't need to replicate this locally — it's mainly useful context if a CI build fails for a target you didn't test.
