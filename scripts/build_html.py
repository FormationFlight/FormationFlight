#Import("env")
import gzip
import io
import os
import sys

from external.minify import (html_minifier, rcssmin, rjsmin)
#import brotli

total_pre_size = 0
total_size = 0
br_bytes_saved = 0
num_files = 0

#env.Execute('"$PYTHONEXE" -m pip install brotli')

# We use our own because mimetypes is broken
# https://bugs.python.org/issue43975
def get_mime_type(file_path):
    ext = os.path.splitext(file_path)[1]
    types = {
        '.js': 'text/javascript',
        '.html': 'text/html',
        '.png': 'image/png',
        '.woff2': 'application/octet-stream',
        '.css': 'text/css',
        '.svg': 'image/svg+xml'
    }
    if ext not in types:
        raise ValueError(f'unable to find mime type for {ext}')
    return types[ext]

def is_text(file_path):
    ext = os.path.splitext(file_path)[1]
    return ext in ['.css', '.html', '.js', '.svg']

def compress_gzip(file_path, data):
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode='wb', compresslevel=9, mtime=0.0) as f:
        f.write(data)
    return buf.getvalue()

def compress(file_path, data):
    return ('gzip', compress_gzip(file_path, data))
    # Brotli over HTTP is not supported - would save us ~15% of code space :(
    brotli_result = brotli.compress(data, mode=(brotli.MODE_TEXT if is_text(file_path) else brotli.MODE_GENERIC))
    gzip_result = compress_gzip(file_path, data)
    if len(gzip_result) < len(brotli_result):
        return ('gzip', gzip_result)
    global br_bytes_saved
    br_bytes_saved += len(gzip_result) - len(brotli_result)
    return ('br', brotli_result)


def minify(file_path):
    if file_path.endswith('.html'):
        with open(file_path, "r") as file:
            return bytes(html_minifier.html_minify(file.read()), 'utf-8')
    # js minify + react = bad
    #if file_path.endswith('.js'):
        # with open(file_path, "r") as file:
        #     return bytes(rjsmin.jsmin(file.read()), 'utf-8')
    if file_path.endswith('.css'):
        with open(file_path, "r") as file:
            return bytes(rcssmin.cssmin(file.read()), 'utf-8')
    with open(file_path, "rb") as file:
        return file.read()

def get_byte_array(file_path):
    file = minify(file_path)
    encoding, byte_data = compress(file_path=file_path, data=file)

    return encoding, bytearray(byte_data)

def generate_c_byte_array(file_path, byte_array):
    file_name = file_path
    array_name = file_name.replace(".", "_").replace("-", "_").replace("/", "_").replace("\\", "_") + "_gz"
    global num_files
    num_files += 1

    c_code = f"static const char PROGMEM {array_name}[] = {{\n"
    for i in range(0, len(byte_array), 16):
        chunk = byte_array[i:i+16]
        hex_values = ", ".join(f"0x{byte:02X}" for byte in chunk)
        c_code += f"    {hex_values},\n"

    c_code += "};\n\n"
    return array_name, c_code

def generate_handler(file_path, file_array_name, encoding):
    file_path = file_path.lstrip("html")
    file_path = file_path.replace("\\", "/")
    mime_type = get_mime_type(file_path)
    handler_base = '''
    server->on("{file_path}", HTTP_GET, [](AsyncWebServerRequest *request) {{
        // Counted like every other handler. Serving the UI's own scripts is the
        // single largest burst of work this server does, and leaving it out
        // would charge it to the main loop on any platform where the server
        // preempts the sketch. See webBusyUs() in WebServer.h.
        ff::WebBusyScope busy;
        AsyncWebServerResponse *response = request->beginResponse_P(200, "{mime_type}", (uint8_t *){file_array_name}, sizeof({file_array_name}));
        response->addHeader("Content-Encoding", "{encoding}");
        request->send(response);
    }});
'''
    result = handler_base.format(file_path=file_path, mime_type=mime_type, file_array_name=file_array_name, encoding=encoding)

    if file_path.endswith('index.html'):
        file_path = file_path.rstrip('index.html')
        result += handler_base.format(file_path=file_path, mime_type=mime_type, file_array_name=file_array_name, encoding=encoding)
    return result

def process_folder(folder_path, isRecursive=False):
    c_code = "#pragma once\n#include <Arduino.h>\n"
    handler_code = ""

    for filename in os.listdir(folder_path):
        file_path = os.path.join(folder_path, filename)
        if os.path.isfile(file_path):
            encoding, byte_array = get_byte_array(file_path)
            array_name, file_c_code = generate_c_byte_array(file_path, byte_array)
            c_code += file_c_code
            handler_code += generate_handler(file_path, array_name, encoding)
            
            global total_pre_size
            pre_size = os.stat(file_path).st_size
            total_pre_size += pre_size
            print(f'{file_path} {pre_size} => {len(byte_array)} bytes')
            global total_size
            total_size += len(byte_array)
            
        elif os.path.isdir(file_path):
            subdir_c_code, subdir_handler_code = process_folder(file_path, True)
            c_code += subdir_c_code
            handler_code += subdir_handler_code
            

    return c_code, handler_code

# Refuse to pack a script that will not parse.
#
# Every file under html/ ends up baked into the firmware image and served from
# flash, and the UI is one ES module graph: a syntax error in any of its files
# takes the whole page down, with nothing to see but a blank tab. A stray
# apostrophe inside a tooltip string did exactly that and was flashed onto two
# boards before anyone noticed, because the check being run was `node --check`
# on a bare .js path, which Node treats as CommonJS and quietly does not parse
# once it meets `import`. Parsing as a module is the only check that means
# anything for these files.
#
# Skipped, with a warning, when there is no node on the PATH: the packer has to
# keep working on a machine that only has PlatformIO.
import shutil
import subprocess

def check_js_syntax(folder):
    node = shutil.which("node")
    if node is None:
        print("warning: node not on PATH, skipping JavaScript syntax check")
        return
    failed = False
    for root, _, names in os.walk(folder):
        for name in sorted(names):
            if not name.endswith(".js"):
                continue
            path = os.path.join(root, name)
            with open(path, "rb") as f:
                result = subprocess.run([node, "--input-type=module", "--check"],
                                        stdin=f, capture_output=True, text=True)
            if result.returncode != 0:
                failed = True
                print(f"JavaScript syntax error in {path}:")
                print(result.stderr.strip())
    if failed:
        print("Refusing to pack html/: fix the errors above.")
        exit(1)

folder_path = 'html'
if not os.path.isdir(folder_path):
    print("Error: The provided path is not a valid directory.")
    exit(1)

check_js_syntax(folder_path)
c_code, handler_code = process_folder(folder_path)

if c_code and handler_code:
    with open("src/hal/webcontent.h", "w") as output_file:
        output_file.write(c_code)
    print(f"HTML content from {num_files} files, {total_pre_size} bytes minified & compressed to {total_size} bytes, written to webcontent.h")
    #print(f"{br_bytes_saved} bytes saved through brotli")

    with open("src/hal/staticfilehandler.inc", "w") as output_file:
        output_file.write(handler_code)
    print(f"HTML handlers for {num_files} files written to staticfilehandler.inc")
else:
    print("No valid files found in the provided directory.")