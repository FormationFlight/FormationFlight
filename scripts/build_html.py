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

folder_path = 'html'
if not os.path.isdir(folder_path):
    print("Error: The provided path is not a valid directory.")
    exit(1)

c_code, handler_code = process_folder(folder_path)

if c_code and handler_code:
    with open("src/lib/WiFi/webcontent.h", "w") as output_file:
        output_file.write(c_code)
    print(f"HTML content from {num_files} files, {total_pre_size} bytes minified & compressed to {total_size} bytes, written to webcontent.h")
    #print(f"{br_bytes_saved} bytes saved through brotli")

    with open("src/lib/WiFi/staticfilehandler.inc", "w") as output_file:
        output_file.write(handler_code)
    print(f"HTML handlers for {num_files} files written to staticfilehandler.inc")
else:
    print("No valid files found in the provided directory.")