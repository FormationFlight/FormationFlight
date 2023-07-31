Import("env")

import io
import gzip
import os
import shutil

from ota_upload import do_upload

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


platform = env.get('PIOPLATFORM', '')
target_name = env['PIOENV'].upper()
# Compress bin files into bin.gz for ESP8266
if platform in ['espressif8266']:
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", compress_bin)
# Replace ESPOTA with our custom HTTP POST OTA
if target_name.endswith('VIA_WIFI'):
    env.Replace(UPLOADCMD=on_upload)
# Copy other files for esp32
if platform in ['espressif32']:
    env.AddPreAction("$BUILD_DIR/${PROGNAME}.bin", copyBootApp0bin)
