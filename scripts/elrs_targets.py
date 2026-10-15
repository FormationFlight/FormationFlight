#!/usr/bin/env python3
'''
FormationFlight on ExpressLRS hardware, using the ExpressLRS targets repo.

ExpressLRS stopped building one firmware per receiver years ago. It builds a few
"unified" images per chip and radio, and the board's pin map rides along in a
fixed-layout trailer appended after the application image:

    [product name 128 B][device name 16 B][options JSON 512 B][layout JSON 2048 B]

FormationFlight now does the same, with the same trailer, so the pin maps come
straight from github.com/ExpressLRS/targets (the `hardware/` submodule) instead of
being copied by hand into an .ini file per board. That also means the ELRS tools
that read and patch the trailer work on a FormationFlight device unchanged.

Not every ELRS board can run FormationFlight yet. That covers ESP8285, ESP32,
ESP32-C3 and ESP32-S3 receivers with an SX127x, SX1280 or LR1121: one radio or
two, with or without a PA/LNA, with or without an antenna switch, as long as
there is a UART to talk MSP over. `supportReasons()` is the
one place that decides, so widening support is a matter of teaching the firmware
the next thing and deleting a line here.

The LoRa band is not part of the image either: it is the radios.lora_band
setting. --band stores the one to boot on in the trailer's options block
("lora-band"), so a freshly flashed node comes up on it. A 900 or LR1121 board
flashed without one boots with LoRa off until a band is chosen in the web UI.
ExpressLRS hardware is matched for 868/915 and 2.4 GHz, so 433 is never offered.

CLI:
    python scripts/elrs_targets.py list [--all]
    python scripts/elrs_targets.py show happymodel.rx_900.es900
    python scripts/elrs_targets.py configure happymodel.rx_900.es900 firmware.bin --band 915 [-o out.bin]

From PlatformIO, set the target (and band) for a Unified_* env and the build
appends them:
    ELRS_TARGET=happymodel.rx_900.es900 ELRS_BAND=915 pio run -e Unified_ESP8285_900_via_UART -t upload
or put `custom_elrs_target` / `custom_lora_band` in an [env:...] section.
'''

import argparse
import json
import os
import struct
import sys

PRODUCT_NAME_SIZE = 128
DEVICE_NAME_SIZE = 16
OPTIONS_SIZE = 512
LAYOUT_SIZE = 2048

ENV_TARGET = 'ELRS_TARGET'
ENV_BAND = 'ELRS_BAND'

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_HARDWARE_DIR = os.path.join(os.path.dirname(HERE), 'hardware')

# ELRS firmware name -> the bands its hardware is matched for. The
# FormationFlight env is the same name without _RX: one image per chip and MCU,
# with the band chosen at runtime.
#
# Never 433. ELRS "900" hardware is an SX127x behind a front end matched for
# 868/915, and an LR1121 board for 868/915 and 2.4 GHz; the chips would tune 433
# but the matching networks are built for twice the frequency.
_SUB = (868, 915)
_DUAL = (868, 915, 2400)
SUPPORTED_FIRMWARE = {
    'Unified_ESP8285_900_RX': _SUB,
    'Unified_ESP8285_2400_RX': (2400,),
    'Unified_ESP32_900_RX': _SUB,
    'Unified_ESP32_2400_RX': (2400,),
    'Unified_ESP32_LR1121_RX': _DUAL,
    'Unified_ESP32C3_900_RX': _SUB,
    'Unified_ESP32C3_2400_RX': (2400,),
    'Unified_ESP32C3_LR1121_RX': _DUAL,
    'Unified_ESP32S3_2400_RX': (2400,),
    'Unified_ESP32S3_LR1121_RX': _DUAL,
}


# Layout keys that mean hardware the firmware cannot drive yet. Each one is a
# reason to refuse rather than to flash something that half works.
UNSUPPORTED_KEYS = {
    'power_apc2': 'PA power set by DAC',
    'power_pdet': 'PA power detector',
    # Left alone, an ELRS VTx comes up wherever its defaults put it, which is
    # not something to find out about in the air.
    'vtx_nss': 'on-board VTx',
}


def loadTargets(hardware_dir=DEFAULT_HARDWARE_DIR):
    path = os.path.join(hardware_dir, 'targets.json')
    if not os.path.exists(path):
        raise SystemExit(f'{path} not found: run `git submodule update --init hardware`')
    with open(path) as f:
        return json.load(f)


def iterProducts(targets):
    '''Yields (dotted path, product dict) for every product in targets.json.'''
    for vendor, vdata in targets.items():
        for category, cdata in vdata.items():
            if category == 'name':
                continue
            for key, product in cdata.items():
                yield f'{vendor}.{category}.{key}', product


def findProduct(targets, dotted):
    parts = dotted.split('.')
    if len(parts) != 3:
        raise SystemExit(f'target "{dotted}" should look like vendor.category.product')
    try:
        return targets[parts[0]][parts[1]][parts[2]]
    except KeyError:
        raise SystemExit(f'target "{dotted}" is not in targets.json')


def layoutPath(hardware_dir, dotted, product):
    category = dotted.split('.')[1]
    sub = 'TX' if category.startswith('tx') else 'RX'
    return os.path.join(hardware_dir, sub, product['layout_file'])


def mergedLayout(hardware_dir, dotted, product):
    '''The layout file with the product's overlay applied, as ELRS does it.'''
    with open(layoutPath(hardware_dir, dotted, product)) as f:
        layout = json.load(f)
    layout.update(product.get('overlay', {}))
    return layout


def supportReasons(hardware_dir, dotted, product):
    '''Why FormationFlight cannot run on this product. Empty means it can.'''
    firmware = product.get('firmware', '')
    if firmware not in SUPPORTED_FIRMWARE:
        return [f'firmware {firmware} (no FormationFlight build for it)']
    if 'layout_file' not in product:
        return ['no layout file']
    layout = mergedLayout(hardware_dir, dotted, product)
    reasons = [why for key, why in UNSUPPORTED_KEYS.items() if key in layout]
    if layout.get('power_control', 0) != 0:
        reasons.append(f'power_control {layout["power_control"]} (only direct control)')
    if 'serial_rx' not in layout or 'serial_tx' not in layout:
        reasons.append('no UART for MSP (PWM-only receiver)')
    if '_900_' in firmware and 'radio_dio0' not in layout:
        reasons.append('SX127x without DIO0')
    if ('_2400_' in firmware or 'LR1121' in firmware) and             ('radio_dio1' not in layout or 'radio_busy' not in layout):
        reasons.append('radio without DIO1/BUSY')
    return reasons


def envFor(product):
    """Unified_ESP8285_900_RX -> Unified_ESP8285_900."""
    return product['firmware'][:-len('_RX')]


def bandsFor(product):
    return SUPPORTED_FIRMWARE[product['firmware']]


def envFirmwareName(env_name):
    """Unified_ESP8285_900_via_UART -> Unified_ESP8285_900_RX, else None."""
    firmware = env_name.split('_via_')[0] + '_RX'
    return firmware if firmware in SUPPORTED_FIRMWARE else None


def findFirmwareEnd(f):
    '''Where the trailer goes: the end of the last image segment, rounded up to
    16 bytes, plus the SHA-256 on ESP32. A straight port of ExpressLRS'
    UnifiedConfiguration.findFirmwareEnd, so the firmware's ESP.getSketchSize()
    lands on the same byte.'''
    f.seek(0, 0)
    (magic, segments, _, _, _) = struct.unpack('<BBBBI', f.read(8))
    if magic != 0xE9:
        raise SystemExit('not an ESP firmware image (bad magic)')
    is8285 = False
    if segments == 2:
        # ESP8266 Arduino images start with eboot; the app image is at 0x1000.
        f.seek(0x1000, 0)
        (magic, segments, _, _, _) = struct.unpack('<BBBBI', f.read(8))
        is8285 = True
    else:
        f.seek(24, 0)
    for _ in range(segments):
        (_, size) = struct.unpack('<II', f.read(8))
        f.seek(size, 1)
    pos = (f.tell() + 16) & ~15
    if not is8285:
        pos += 32
    return pos


def fixed(text, size):
    return (text.encode() + b'\0' * size)[:size]


def buildTrailer(product, layout, options=None):
    layout_json = json.dumps(layout, separators=(',', ':')).encode()
    if len(layout_json) >= LAYOUT_SIZE:
        raise SystemExit(f'layout is {len(layout_json)} bytes, the trailer holds {LAYOUT_SIZE - 1}')
    options_json = json.dumps(options or {}, separators=(',', ':'))
    return (fixed(product['product_name'], PRODUCT_NAME_SIZE) +
            fixed(product.get('lua_name', ''), DEVICE_NAME_SIZE) +
            fixed(options_json, OPTIONS_SIZE) +
            fixed(layout_json.decode(), LAYOUT_SIZE))


def configure(bin_path, dotted, out_path=None, hardware_dir=DEFAULT_HARDWARE_DIR,
              env_name=None, force=False, band=None, name=None, passphrase=None):
    '''Writes the trailer for `dotted` into a copy of bin_path (or in place).'''
    targets = loadTargets(hardware_dir)
    product = findProduct(targets, dotted)
    reasons = supportReasons(hardware_dir, dotted, product)
    if reasons and not force:
        raise SystemExit(f'{dotted} ({product["product_name"]}) is not supported yet: ' +
                         '; '.join(reasons))
    if env_name is not None:
        want = envFirmwareName(env_name)
        if want != product['firmware']:
            raise SystemExit(f'{dotted} needs the {envFor(product)} build, not {env_name}')
    bands = bandsFor(product)
    options = {}
    if band:
        band = int(band)
        if band not in bands:
            raise SystemExit(f'{dotted} is matched for {"/".join(map(str, bands))} MHz, '
                             f'not {band}')
        options['lora-band'] = band
    elif len(bands) == 1:
        options['lora-band'] = bands[0]
    # First-boot settings, applied by a node that has no config.json yet.
    if name:
        if len(name) > 15:
            raise SystemExit('--name is at most 15 characters')
        options['name'] = name
    if passphrase:
        if len(passphrase) > 32:
            raise SystemExit('--passphrase is at most 32 characters')
        options['group-passphrase'] = passphrase
    layout = mergedLayout(hardware_dir, dotted, product)
    trailer = buildTrailer(product, layout, options)

    with open(bin_path, 'rb') as f:
        end = findFirmwareEnd(f)
        f.seek(0)
        image = f.read(end)
    # Truncating at the computed end makes this idempotent: configuring an
    # already-configured image replaces its trailer instead of stacking another.
    image = image + b'\xff' * (end - len(image))
    with open(out_path or bin_path, 'wb') as f:
        f.write(image + trailer)
    return product


def cmdList(args):
    targets = loadTargets(args.hardware)
    rows = []
    for dotted, product in iterProducts(targets):
        if not dotted.split('.')[1].startswith('rx'):
            continue
        reasons = supportReasons(args.hardware, dotted, product)
        if reasons and not args.all:
            continue
        envs = (f'{envFor(product)}, {"/".join(map(str, bandsFor(product)))}'
                if not reasons else '-')
        rows.append((dotted, product['product_name'], envs, '; '.join(reasons)))
    width = max((len(r[0]) for r in rows), default=0)
    for dotted, name, envs, why in sorted(rows):
        line = f'{dotted:<{width}}  {name}  [{envs}]'
        if why:
            line += f'  -- {why}'
        print(line)
    print(f'\n{sum(1 for r in rows if not r[3])} supported', file=sys.stderr)


def cmdShow(args):
    targets = loadTargets(args.hardware)
    product = findProduct(targets, args.target)
    print(json.dumps(product, indent=2))
    if 'layout_file' in product:
        print(json.dumps(mergedLayout(args.hardware, args.target, product), indent=2))
    reasons = supportReasons(args.hardware, args.target, product)
    print('supported: ' + (f'yes, build {envFor(product)}, bands '
                           f'{"/".join(map(str, bandsFor(product)))}' if not reasons
                           else 'no -- ' + '; '.join(reasons)))


def cmdConfigure(args):
    product = configure(args.firmware, args.target, args.output, args.hardware,
                        force=args.force, band=args.band, name=args.name,
                        passphrase=args.passphrase)
    print(f'{args.output or args.firmware}: configured for {product["product_name"]}')
    if not args.band and len(bandsFor(product)) > 1:
        print('no --band: LoRa stays off until a band is chosen in the web UI')


def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    p.add_argument('--hardware', default=DEFAULT_HARDWARE_DIR,
                   help='ExpressLRS targets checkout (default: the hardware/ submodule)')
    sub = p.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('list', help='ELRS receivers FormationFlight can run on')
    s.add_argument('--all', action='store_true', help='include unsupported ones, with why')
    s.set_defaults(func=cmdList)
    s = sub.add_parser('show', help='one product, its merged layout, and whether it is supported')
    s.add_argument('target')
    s.set_defaults(func=cmdShow)
    s = sub.add_parser('configure', help='append a product\'s trailer to a unified firmware.bin')
    s.add_argument('target')
    s.add_argument('firmware')
    s.add_argument('-o', '--output')
    s.add_argument('--band', type=int, help='LoRa band to boot on: 868, 915 or 2400')
    s.add_argument('--name', help='node name, applied on first boot')
    s.add_argument('--passphrase', help='group passphrase, applied on first boot')
    s.add_argument('--force', action='store_true', help='configure an unsupported product anyway')
    s.set_defaults(func=cmdConfigure)
    args = p.parse_args()
    args.func(args)


if __name__ == '__main__':
    main()
