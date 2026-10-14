Import("env")

import io
import gzip
import json
import os
import re
import shutil
import sys

from ota_upload import do_upload
import elrs_targets

def compress_gzip(data):
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode='wb', compresslevel=9, mtime=0.0) as f:
        f.write(data)
    return buf.getvalue()

def compress_bin(source, target, env):
    build_dir = env.subst("$BUILD_DIR")
    image_name = env.subst("$PROGNAME")
    source_file = os.path.join(build_dir, image_name + ".bin")
    target_file = source_file + ".gz"
    with open(source_file, 'rb') as input:
        with gzip.GzipFile(target_file, mode='wb', compresslevel=9, mtime=0.0) as output:
            output.write(input.read())
            output.close()
            print(f'Wrote compressed firmware to {target_file}')

def on_upload(source, target, env):
    firmware_path = str(source[0])
    bin_path = os.path.dirname(firmware_path)
    upload_addr = 'http://192.168.4.1/update'
    # Try the gzipped version first
    bin_target = os.path.join(bin_path, 'firmware.bin.gz')
    if not os.path.exists(bin_target):
        bin_target = os.path.join(bin_path, 'firmware.bin')
    if not os.path.exists(bin_target):
        raise Exception("No valid binary found!")
    print(f'Uploading {bin_target} to {upload_addr}...')
    with open(bin_target, 'rb') as f:
        return do_upload(upload_addr, 'firmware.bin', f.read())
    
def copyBootApp0bin(source, target, env):
    file = os.path.join(env.PioPlatform().get_package_dir("framework-arduinoespressif32"), "tools", "partitions", "boot_app0.bin")
    shutil.copy2(file, os.path.join(env['PROJECT_BUILD_DIR'], env['PIOENV']))


def elrsTarget():
    '''The ExpressLRS product to configure a unified image for: ELRS_TARGET from
    the environment, else custom_elrs_target from the env section.'''
    target = os.environ.get(elrs_targets.ENV_TARGET, '').strip()
    if not target:
        target = env.GetProjectOption('custom_elrs_target', '').strip()
    return target

def loraBand():
    """The band to boot on: ELRS_BAND, else custom_lora_band, else none."""
    band = os.environ.get(elrs_targets.ENV_BAND, '').strip()
    if not band:
        band = env.GetProjectOption('custom_lora_band', '').strip()
    return band or None

def configure_unified(source, target, env):
    build_dir = env.subst("$BUILD_DIR")
    bin_path = os.path.join(build_dir, env.subst("$PROGNAME") + ".bin")
    product = elrs_targets.configure(bin_path, elrsTarget(), env_name=env['PIOENV'],
                                     band=loraBand())
    print(f'Configured {bin_path} for {product["product_name"]}')
    if not loraBand() and len(elrs_targets.bandsFor(product)) > 1:
        print(f'No {elrs_targets.ENV_BAND}: LoRa stays off until a band is chosen in the web UI')


def cppdefine(name):
    """A -D value from this env's build flags, unquoted; True for a bare -D;
    None when absent. Read from the flags as written, because a quoted
    '-D NAME="two words"' never makes it into CPPDEFINES as a pair."""
    flags = ' '.join(str(f) for f in env.get('BUILD_FLAGS', []))
    m = re.search(r'-D\s*' + re.escape(name) + r'(?:=("[^"]*"|\S+))?(?=\s|\'|$)', flags)
    if not m:
        return None
    if m.group(1) is None:
        return True
    return m.group(1).strip('"').replace('\\"', '')


def package_release(source, target, env):
    """Everything the web installer needs to flash this env, under release
    names, in $BUILD_DIR/release:

        <env>.bin             the application image
        <env>.bootloader.bin  ESP32 family only
        <env>.partitions.bin  ESP32 family only
        boot_app0.bin         ESP32 family only; the same for every env
        <env>.flash.json      where each of those goes, and what the board is

    Offsets come from PlatformIO's own upload command (FLASH_EXTRA_IMAGES and
    ESP32_APP_OFFSET), so they are whatever `pio run -t upload` would have used:
    the bootloader is at 0x1000 on an ESP32 and at 0x0 on a C3 or S3.
    """
    build_dir = env.subst("$BUILD_DIR")
    out = os.path.join(build_dir, "release")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    name = re.sub('_via_.*', '', env['PIOENV'])
    app = os.path.join(build_dir, env.subst("$PROGNAME") + ".bin")
    shutil.copy2(app, os.path.join(out, name + ".bin"))

    parts = []
    if platform == 'espressif32':
        mcu = env.BoardConfig().get("build.mcu", "esp32")
        for offset, path in env.get("FLASH_EXTRA_IMAGES", []):
            path = env.subst(path)
            base = os.path.basename(path)
            if base.startswith("bootloader"):
                release_name = name + ".bootloader.bin"
            elif base.startswith("partitions"):
                release_name = name + ".partitions.bin"
            else:
                release_name = base  # boot_app0.bin, shared by every env
            shutil.copy2(path, os.path.join(out, release_name))
            parts.append({"file": release_name, "offset": int(env.subst(offset), 0)})
        app_offset = int(env.subst("$ESP32_APP_OFFSET"), 0)
    else:
        mcu = "esp8266"
        app_offset = 0  # the image starts with eboot, at 0x0
    parts.append({"file": name + ".bin", "offset": app_offset, "app": True})

    radio = None
    for family, label in (('LORA_FAMILY_LR1121', 'LR1121'), ('LORA_FAMILY_SX128X', 'SX1280'),
                          ('LORA_FAMILY_SX127X', 'SX127x')):
        if cppdefine(family):
            radio = label
            break
    default_band = cppdefine('LORA_DEFAULT_BAND')
    info = {
        "env": name,
        "mcu": mcu,
        "unified": bool(cppdefine('UNIFIED_TARGET')),
        "name": cppdefine('CFG_TARGET_FULLNAME') or name,
        "radio": radio,
        "default_band": int(default_band) if default_band else None,
        "parts": sorted(parts, key=lambda p: p["offset"]),
    }
    with open(os.path.join(out, name + ".flash.json"), "w") as f:
        json.dump(info, f, indent=2)
    print(f'Release files for the web installer in {out}')

    if platform == 'espressif8266':
        check_ota_fits(app)


def ldscript_fs_start():
    """Flash offset where this env's filesystem starts, from its linker script,
    or None if the script cannot be found or does not say."""
    script = env.GetProjectOption('board_build.ldscript', '') or \
        env.BoardConfig().get('build.arduino.ldscript', '')
    candidates = [os.path.join(env.subst('$PROJECT_DIR'), script),
                  os.path.join(env.PioPlatform().get_package_dir('framework-arduinoespressif8266'),
                               'tools', 'sdk', 'ld', script)]
    for path in candidates:
        if os.path.isfile(path):
            m = re.search(r'_FS_start\s*=\s*(0x[0-9a-fA-F]+)', open(path).read())
            if m:
                return int(m.group(1), 16) - 0x40200000
    return None


def check_ota_fits(app):
    """Fail the build when this image can no longer be updated over WiFi.

    The ESP8266 Updater stages an upload between the end of the running sketch
    (rounded up to a 4 KB sector) and the start of the filesystem, and refuses
    one that does not fit. On a 1 MB part only the .bin.gz ever fits, so that
    is what is checked, at the larger of zlib level 9 (what this build writes)
    and 6 (roughly what a browser's CompressionStream produces, which is how
    the web installer makes one). It is the image installed now that needs the
    room, and the next one that has to fit: checking one image against itself
    assumes successive builds stay about the same size, which they do.
    """
    fs_start = ldscript_fs_start()
    if fs_start is None:
        print('OTA size check skipped: could not read _FS_start from the linker script')
        return
    data = open(app, 'rb').read()
    room = fs_start - (len(data) + 0xFFF) // 0x1000 * 0x1000
    gz = max(len(gzip.compress(data, 9)), len(gzip.compress(data, 6)))
    if gz > room:
        sys.stderr.write(
            f'\n*** OTA no longer fits: the .bin.gz is {gz // 1024} KB and a node running '
            f'this image has {room // 1024} KB to receive one in. WiFi updates would fail. '
            f'Shrink the image, or the filesystem in the linker script.\n\n')
        env.Exit(1)
    print(f'OTA: .bin.gz {gz // 1024} KB of {room // 1024} KB available ({(room - gz) // 1024} KB spare)')


platform = env.get('PIOPLATFORM', '')
target_name = env['PIOENV'].upper()
# Unified images get their board's layout appended. Registered first so the
# ESP8266 .bin.gz below is made from the configured image.
if elrs_targets.envFirmwareName(env['PIOENV']) is not None:
    if elrsTarget():
        env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", configure_unified)
    else:
        print(f'{env["PIOENV"]}: no {elrs_targets.ENV_TARGET} set, building a bare unified image '
              '(see `python scripts/elrs_targets.py list`)')
# Compress bin files into bin.gz for ESP8266
if platform in ['espressif8266']:
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", compress_bin)
# The web installer's files. Last, so it packages the image as finally built.
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", package_release)
# Replace ESPOTA with our custom HTTP POST OTA
if target_name.endswith('VIA_WIFI'):
    env.Replace(UPLOADCMD=on_upload)
# Copy other files for esp32
if platform in ['espressif32']:
    env.AddPreAction("$BUILD_DIR/${PROGNAME}.bin", copyBootApp0bin)
