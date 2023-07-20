import gzip
import shutil
import os
import requests
import sys
import io

ENDPOINT_URL = 'http://192.168.4.1'

def determine_platform():
    response = requests.get(ENDPOINT_URL + "/system/status")
    if response.status_code == 200:
        return response.json()['platform']
    return 'unknown'

def read_to_memory(input_file_path):
    with open(input_file_path, 'rb') as f_in:
        # Create an in-memory buffer to hold the gzipped content
        buffer = io.BytesIO(f_in.read())
        buffer.seek(0)
        return buffer

def gzip_compress_in_memory(input_file_path):
    with open(input_file_path, 'rb') as f_in:
        # Create an in-memory buffer to hold the gzipped content
        buffer = io.BytesIO()
        # Check header
        header = f_in.read(2)
        f_in.seek(0)
        if header[0] == 0x1F and header[1] == 0x8B:
            print('Already gzipped, sending unmodified file')
            # Create an in-memory buffer to hold the raw content
            buffer = io.BytesIO(f_in.read())
            buffer.seek(0)
            return buffer

        with gzip.GzipFile(fileobj=buffer, mode='wb') as f_out:
            f_out.writelines(f_in)
        
        # Move the buffer's position to the start
        buffer.seek(0)
        print(f"Compressed to {buffer.getbuffer().nbytes} bytes")
        return buffer

def post_gzipped_file(url, filename, file_buffer):
    files = {'file': (filename, file_buffer)}
    response = requests.post(url, files=files)
    return response

if __name__ == "__main__":
    input_file_path = sys.argv[1]
    print(f"Determining target...")
    platform = determine_platform()
    print(f"Connected to a(n) {platform}")
    # Read in the file as written
    file_buffer = read_to_memory(input_file_path)
    if platform == 'ESP8266':
        # Gzip compress the input file and keep it in memory
        file_buffer = gzip_compress_in_memory(input_file_path)
    print(f"Sending {os.path.basename(input_file_path)} ({file_buffer.getbuffer().nbytes} bytes) to {ENDPOINT_URL}...")

    # Make the HTTP POST request
    response = post_gzipped_file(ENDPOINT_URL + '/update', "firmware.bin", file_buffer)

    # Check the response
    if response.status_code == 200:
        print(f"File upload successful!\n{response.text}")
    else:
        print(f"File upload failed with status code: {response.status_code}\n{response.text}")