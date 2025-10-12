# FormationFlight MSP Bridge

The FormationFlight MSP Bridge is a Python application that acts as an intermediary between FormationFlight modules and a web-based interface. It communicates with the module via MSP (Multiwii Serial Protocol) over a serial connection, and provides real-time  data to connected clients via HTTP and WebSocket APIs.

## Features

- **Serial Communication**: Connects to FormationFlight modules via serial ports to send/receive MSP messages.
- **Web Interface**: Serves a static web application for visualizing drone formations.
- **Real-time Streaming**: Broadcasts live data updates to WebSocket clients.
- **REST API**: Provides system status information via HTTP endpoints.
- **Peer Management**: Tracks and labels multiple drones in the network.
- **Auto-connect**: Supports automatic serial connection on startup.

## Installation

1. Ensure Python 3.8+ is installed.
2. Create a virtual environment:
   ```python3 -m venv venv source venv/bin/activate```  # On Windows: venv\Scripts\activate
   
3. Install dependencies:
   ```pip install websockets pyserial```

## Usage

Run the bridge with default settings:
```python3 bridge.py```

### Command-Line Options

- `--http-port PORT`: HTTP server port (default: 8080)
- `--ws-host HOST`: WebSocket bind address (default: 0.0.0.0)
- `--ws-port PORT`: WebSocket port (default: 8766)
- `--device-name NAME`: Name exposed via /system/status (default: "MSP Bridge")
- `--log-level LEVEL`: Logging level (DEBUG, INFO, WARNING, ERROR; default: INFO)
- `--no-websocket`: Disable WebSocket streaming
- `--peer-map RAWID=LABEL[:NAME]`: Override peer slot labels/display names (can be used multiple times)
- `--peer-map-file FILE`: JSON file containing peer label/name overrides
- `--auto-connect PORT`: Serial port to connect automatically on startup
- `--baud BAUD`: Serial baud rate for auto-connect (default: 115200)

### Examples

Start with debug logging and auto-connect to a serial port:
```python3 bridge.py --log-level DEBUG --auto-connect /dev/ttyUSB0```

Use a custom peer mapping:
```python3 bridge.py --peer-map 1=Drone1 --peer-map 2=Drone2:MyDrone```

## APIs

### HTTP API

The bridge serves static files from the `web/` directory and provides the following endpoints:

#### GET /system/status

Returns system information in JSON format.

**Response:**
```json
{
  "target": "FormationFlight MSP Bridge",
  "platform": "Linux",
  "version": "0.3.0",
  "gitHash": "",
  "buildTime": "",
  "uptimeMilliseconds": 12345,
  "name": "MSP Bridge",
  "serial": {
    "connected": true,
    "port": "/dev/ttyUSB0",
    "baud": 115200
  }
}
```

### WebSocket API

Connect to `ws://localhost:8766` (or configured host/port) to receive real-time updates.

Messages are JSON objects with a `type` and `payload`.

#### Peers Message

Sent when peer data is updated.

**Format:**
```json
{
  "type": "peers",
  "payload": {
    "myID": "X",
    "peers": [
      {
        "rawId": 1,
        "id": "A",
        "name": "Drone A",
        "updated": 1640995200000,
        "age": 0,
        "lost": 0,
        "lat": 37.7749,
        "lon": -122.4194,
        "alt": 100.5,
        "groundSpeed": 15.2,
        "groundCourse": 90.0,
        "distance": 0.0,
        "lq": 95
      }
    ]
  }
}
```

#### Serial Message

Sent when serial connection status changes.

**Format:**
```json
{
  "type": "serial",
  "payload": {
    "connected": true,
    "port": "/dev/ttyUSB0",
    "baud": 115200
  }
}
```

## Architecture

- **SerialManager**: Handles serial communication with the flight controller, parsing MSP messages.
- **BridgeState**: Maintains application state (peers, GNSS, system info).
- **WebSocketBroadcaster**: Manages WebSocket connections and broadcasts updates.
- **HTTP Server**: Serves the web interface and API endpoints.

## Troubleshooting

- **Serial Connection Issues**: Ensure the correct port and baud rate. Use `--log-level DEBUG` for detailed logs.
- **WebSocket Connection Fails**: Check firewall settings and ensure the WebSocket port is accessible.
- **No Data Updates**: Verify the FormationFlight module is sending MSP data and the serial connection is stable.

## Contributing

This is part of the FormationFlight project. See the main repository for contribution guidelines.