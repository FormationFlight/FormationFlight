#!/usr/bin/env python3
'''
Builds index.json for the web installer (formationflight.org/installer/).

Input is a directory of release files, as CI collects them from every env's
$BUILD_DIR/release (see package_release() in env_setup.py): per env an
<env>.bin, an <env>.flash.json, and on the ESP32 family a bootloader and a
partition table. Output is one index.json describing everything the installer
can flash from that release:

    envs     per build: chip, radio, and which file goes where
    devices  per board someone would pick from a list: its vendor, its name,
             which env to flash, which LoRa bands it may run, and -- for
             ExpressLRS hardware -- the layout to append to the image

The installer appends that layout in the browser, in the same trailer
scripts/elrs_targets.py writes, so one Unified_* image serves every board.

    python scripts/flasher_manifest.py <release dir> --version v6.0.0 [--commit abc1234]
'''

import argparse
import datetime
import glob
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import elrs_targets  # noqa: E402

CHIP_NAMES = {
    'esp8266': 'ESP8266',
    'esp32': 'ESP32',
    'esp32c3': 'ESP32-C3',
    'esp32s3': 'ESP32-S3',
    'esp32s2': 'ESP32-S2',
}

# The bands a hand-written target's chip can run. Unlike ExpressLRS hardware,
# these boards come in band-specific variants (a 433 Lilygo is a 433 board),
# so each variant's own band is what is offered; see legacyDevices().
_BAND_RE = re.compile(r'\s*\(?(433|868|915|2400)\s*MHz\)?\s*$', re.I)


def loadEnvs(release_dir):
    envs = {}
    for path in sorted(glob.glob(os.path.join(release_dir, '*.flash.json'))):
        with open(path) as f:
            info = json.load(f)
        for part in info['parts']:
            if not os.path.exists(os.path.join(release_dir, part['file'])):
                raise SystemExit(f'{path} names {part["file"]}, which is not in {release_dir}')
        info['chip'] = CHIP_NAMES.get(info['mcu'], info['mcu'].upper())
        envs[info['env']] = info
    return envs


def vendorFor(env, name):
    lower = (env + ' ' + name).lower()
    if 'lilygo' in lower or 't-beam' in lower:
        return 'LilyGO'
    if 'heltec' in lower:
        return 'Heltec'
    if 'adafruit' in lower:
        return 'Adafruit'
    if 'wemos' in lower or 'lolin' in lower:
        return 'Wemos / Lolin'
    return 'Generic'


def elrsDevices(envs, hardware_dir):
    targets = elrs_targets.loadTargets(hardware_dir)
    devices = []
    for dotted, product in elrs_targets.iterProducts(targets):
        vendor_key, category, _ = dotted.split('.')
        if not category.startswith('rx'):
            continue
        if elrs_targets.supportReasons(hardware_dir, dotted, product):
            continue
        env = elrs_targets.envFor(product)
        if env not in envs:
            continue  # a partial build: not every env made it into this release
        bands = list(elrs_targets.bandsFor(product))
        devices.append({
            'id': dotted,
            'vendor': targets[vendor_key].get('name', vendor_key),
            'name': product['product_name'],
            'group': 'ExpressLRS receivers',
            'envs': {str(b): env for b in bands},
            'bands': bands,
            'default_band': bands[0] if len(bands) == 1 else None,
            'elrs': {
                'product_name': product['product_name'],
                'lua_name': product.get('lua_name', ''),
                'layout': elrs_targets.mergedLayout(hardware_dir, dotted, product),
            },
        })
    return devices


def legacyDevices(envs):
    '''The hand-written targets, with band variants of one board folded into a
    single device: "Lilygo v1.0 433MHz", "... 868MHz" and "... 915MHz" are one
    board to pick, and a band to pick for it.'''
    boards = {}
    for env, info in envs.items():
        if info['unified']:
            continue
        name = _BAND_RE.sub('', info['name']).strip() or env
        radio = info.get('radio')
        key = (name, info['chip'], radio)
        board = boards.setdefault(key, {
            'id': 'ff.' + re.sub(r'_(433|868|915|2400)$', '', env),
            'vendor': vendorFor(env, name),
            'name': name,
            'group': 'LoRa boards' if radio else 'ESP-NOW only',
            'envs': {},
            'bands': [],
            'default_band': None,
            'elrs': None,
        })
        band = info.get('default_band')
        if band:
            board['envs'][str(band)] = env
            board['bands'] = sorted(set(board['bands']) | {band})
        else:
            board['envs']['0'] = env
    for board in boards.values():
        if len(board['bands']) == 1:
            board['default_band'] = board['bands'][0]
    return sorted(boards.values(), key=lambda d: (d['vendor'], d['name']))


def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    p.add_argument('release_dir')
    p.add_argument('--version', required=True)
    p.add_argument('--commit', default='')
    p.add_argument('--hardware', default=elrs_targets.DEFAULT_HARDWARE_DIR)
    p.add_argument('-o', '--output', help='default: <release_dir>/index.json')
    args = p.parse_args()

    envs = loadEnvs(args.release_dir)
    if not envs:
        raise SystemExit(f'no *.flash.json in {args.release_dir}')
    devices = elrsDevices(envs, args.hardware) + legacyDevices(envs)
    index = {
        'schema': 1,
        'version': args.version,
        'commit': args.commit,
        'date': datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
        'envs': envs,
        'devices': devices,
    }
    out = args.output or os.path.join(args.release_dir, 'index.json')
    with open(out, 'w') as f:
        json.dump(index, f, separators=(',', ':'))
    print(f'{out}: {len(envs)} builds, {len(devices)} devices '
          f'({sum(1 for d in devices if d["elrs"])} ExpressLRS)')


if __name__ == '__main__':
    main()
