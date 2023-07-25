#Import("env")
import gzip
import io
import mimetypes
import os
import sys

total_size = 0
num_files = 0

def compress(data):
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode='wb', compresslevel=9, mtime=0.0) as f:
        f.write(data)
    return buf.getvalue()

def get_byte_array(file_path):
    with open(file_path, "rb") as file:
        byte_data = compress(file.read())
        return bytearray(byte_data)

def generate_c_byte_array(file_path, byte_array):
    file_name = (file_path)
    array_name = file_name.replace(".", "_").replace("-", "_").replace("/", "_").replace("\\", "_") + "_gz"
    global total_size
    total_size += len(byte_array)
    global num_files
    num_files += 1

    c_code = f"static const char PROGMEM {array_name}[] = {{\n"
    for i in range(0, len(byte_array), 16):
        chunk = byte_array[i:i+16]
        hex_values = ", ".join(f"0x{byte:02X}" for byte in chunk)
        c_code += f"    {hex_values},\n"

    c_code += "};\n\n"
    return array_name, c_code

def process_folder(folder_path, isRecursive=False):
    c_code = "#pragma once\n#include <Arduino.h>\n"
    for filename in os.listdir(folder_path):
        file_path = os.path.join(folder_path, filename)
        if os.path.isfile(file_path):
            byte_array = get_byte_array(file_path)
            c_code += generate_c_byte_array(file_path, byte_array)
        elif os.path.isdir(file_path):
            c_code += process_folder(file_path, True)

    return c_code

def generate_handler(file_path, file_array_name):
    file_path = file_path.lstrip("html")
    file_path = file_path.replace("\\", "/")
    mime_type, _ = mimetypes.guess_type(file_path)
    if mime_type is None:
        mime_type = "application/octet-stream"
    handler_base = '''
    server->on("{file_path}", HTTP_GET, [](AsyncWebServerRequest *request) {{
        AsyncWebServerResponse *response = request->beginResponse_P(200, "{mime_type}", (uint8_t *){file_array_name}, sizeof({file_array_name}));
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    }});
'''
    result = handler_base.format(file_path=file_path, mime_type=mime_type, file_array_name=file_array_name)

    if file_path.endswith('index.html'):
        file_path = file_path.rstrip('index.html')
        result += handler_base.format(file_path=file_path, mime_type=mime_type, file_array_name=file_array_name)
    return result

def process_folder(folder_path, isRecursive=False):
    c_code = "#pragma once\n#include <Arduino.h>\n"
    handler_code = ""

    for filename in os.listdir(folder_path):
        file_path = os.path.join(folder_path, filename)
        if os.path.isfile(file_path):
            byte_array = get_byte_array(file_path)
            array_name, file_c_code = generate_c_byte_array(file_path, byte_array)
            c_code += file_c_code
            handler_code += generate_handler(file_path, array_name)
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
    print(f"HTML content from {num_files} files compressed to {total_size} bytes and written to webcontent.h")
    with open("src/lib/WiFi/staticfilehandler.inc", "w") as output_file:
        output_file.write(handler_code)
    print(f"HTML handlers for {num_files} files written to staticfilehandler.inc")
else:
    print("No valid files found in the provided directory.")