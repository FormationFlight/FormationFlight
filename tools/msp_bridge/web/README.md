# FormationFlight MSP Bridge Web Interface

This web interface provides a real-time map display for FormationFlight, allowing you to visualize drones in your FormationFlight network.

## Setup

1. **Create a Python Virtual Environment**:
   ```python3 -m venv venv```

2. **Activate the Virtual Environment**:
   - On Linux/macOS:
     ```source venv/bin/activate```
   - On Windows:
     ```venv\Scripts\activate```

3. **Install Dependencies**:
   ```pip install websockets pyserial```

## Features

- **Interactive Map**: Displays peer drones and the GCS (Ground Control Station) location using OpenLayers.
  - Drones are shown with blue icons.
  - The GCS (self) is shown with a green controller icon, if it is identified.
- **Real-time Updates**: Receives live data from the MSP bridge via WebSocket, updating positions, altitudes, speeds, etc.
- **Peer Information**: Hover over drone markers to see more detailed information including ID, name, coordinates, altitude, ground speed, heading, distance, and link quality.
- **Connection Status**: Visual indicator showing connection state (connected/disconnected).
- **Peer Count**: Displays the current number of active peers.
- **Serial Port Control**: Input field to specify the serial port for connecting to the flight controller.

## How to Use

1. **Start the Bridge**: Run the MSP bridge server (`python3 bridge.py`) which serves this web interface and handles WebSocket connections.  Make sure your venv is activated first.

2. **Access the Interface**: Open your web browser and navigate to `http://localhost:8080` (or the configured HTTP port).

3. **Connect to FormationFLight Receiver**:
   - Connect ExpresLRS receiver flashed with FormationFlight to your PC via serial-USB adapter.
   - (optional) Connect to the module over wifi and force ground mode.
   - Enter the serial port (e.g., `/dev/ttyUSB0` on Linux) in the "Serial Port" field.
   - Click "Connect" to establish a connection to the flight controller.
   - The status dot will turn green when connected.

4. **View the Map**:
   - The map will automatically center on the first peer or GCS location.
   - Markers update in real-time as new data arrives.
   - Use mouse to pan and zoom the map.

5. **Monitor Peers**:
   - The peer count in the header shows how many drones are active.
   - Hover over markers for detailed tooltips.

6. **Disconnect**: Click "Disconnect" to close the serial connection.

## Technical Details

- Built with OpenLayers for mapping.
- Uses WebSocket for real-time data from the bridge.
- Icons sourced from Reshot (adapted for custom colors).  Attribution is in index.html

## Requirements

- Web browser with WebSocket support.
- Running FormationFlight MSP bridge server (bridge.py).
- Serial connection to FormationFlight receiver/module.
