#!/usr/bin/env python3

"""FormationFlight MSP to REST and Websocket bridge"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import math
import mimetypes
import platform
import signal
import struct
import sys
import threading
import time
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple, Union
from urllib.parse import parse_qs, urlsplit

try:
    import serial
    from serial import SerialException
except ImportError:
    serial = None
    SerialException = Exception

try:
    import websockets
except ImportError:
    websockets = None


BRIDGE_VERSION = "0.3.0"
DEFAULT_HTTP_PORT = 8080
DEFAULT_SERIAL_BAUD = 115200
DEFAULT_SERIAL_PORT = None
DEFAULT_WS_HOST = "0.0.0.0"
DEFAULT_WS_PORT = 8766
BASE_DIR = Path(__file__).resolve().parent
DEFAULT_STATIC_DIR = BASE_DIR / "web"

MSP_FC_VARIANT = 2
MSP2_COMMON_SET_RADAR_POS = 0x100B


PEER_SLOT_NAMES = ["X", "A", "B", "C", "D", "E", "F", "G", "H"]
NODES_MAX = 6
LORA_PEER_TIMEOUT_MS = 6000


def ms_now() -> int:
    return int(time.time() * 1000)


def crc8_dvb_s2(crc: int, value: int) -> int:
    crc ^= value
    for _ in range(8):
        if crc & 0x80:
            crc = ((crc << 1) ^ 0xD5) & 0xFF
        else:
            crc = (crc << 1) & 0xFF
    return crc


def horizontal_distance_m(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    radius = 6371000.0
    phi1, phi2 = math.radians(lat1), math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlambda = math.radians(lon2 - lon1)
    a = math.sin(dphi / 2.0) ** 2 + math.cos(phi1) * math.cos(phi2) * math.sin(dlambda / 2.0) ** 2
    c = 2.0 * math.atan2(math.sqrt(a), math.sqrt(1.0 - a))
    return radius * c


@dataclass
class PeerRecord:
    peer_id: int
    state: int = 0
    lat: float = 0.0
    lon: float = 0.0
    alt_m: float = 0.0
    heading_deg: float = 0.0
    speed_cms: float = 0.0
    lq: int = 0
    last_update_ms: int = 0
    packets_received: int = 0

    def mark_update(
        self,
        lat_deg: float,
        lon_deg: float,
        alt_cm: float,
        heading_deg: float,
        speed_cms: float,
        link_quality: int,
        state: int,
    ) -> None:
        self.lat = lat_deg
        self.lon = lon_deg
        self.alt_m = alt_cm / 100.0
        self.heading_deg = heading_deg
        self.speed_cms = speed_cms
        self.lq = link_quality
        self.state = state
        self.last_update_ms = ms_now()
        self.packets_received += 1

    def json(
        self,
        self_location: Optional[Any],
        now_ms: int,
        id_label: str,
        display_name: str,
    ) -> Dict[str, object]:
        age = max(0, now_ms - self.last_update_ms)
        lost = 2 if age > LORA_PEER_TIMEOUT_MS else 0
        distance = 0.0
        if self_location and self_location.fix_type != 0:
            distance = horizontal_distance_m(
                self_location.lat, self_location.lon, self.lat, self.lon
            )
        payload: Dict[str, object] = {
            "rawId": self.peer_id,
            "id": id_label,
            "name": display_name,
            "updated": self.last_update_ms,
            "age": age,
            "lost": lost,
            "lat": self.lat,
            "lon": self.lon,
            "alt": self.alt_m,
            "groundSpeed": self.speed_cms,
            "groundCourse": self.heading_deg * 10.0,
            "distance": distance,
            "lq": self.lq,
        }
        return payload


class BridgeState:
    def __init__(self, device_name: str, peer_aliases: Optional[Dict[int, Tuple[str, str]]] = None) -> None:
        self._lock = threading.RLock()
        self.start_ms = ms_now()
        self.device_name = device_name
        self.peers: Dict[int, PeerRecord] = {}
        self.peer_aliases = peer_aliases or {}
        self._broadcaster: Optional["WebSocketBroadcaster"] = None
        self.serial_connected = False
        self.serial_port: Optional[str] = None
        self.serial_baud: int = DEFAULT_SERIAL_BAUD
        self.peer_updates = 0

    def set_broadcaster(self, broadcaster: "WebSocketBroadcaster") -> None:
        self._broadcaster = broadcaster

    def set_serial_connection(self, connected: bool, port: Optional[str], baud: int) -> None:
        with self._lock:
            self.serial_connected = connected
            self.serial_port = port
            self.serial_baud = baud
            snapshot = {
                "connected": connected,
                "port": port,
                "baud": baud,
            }
        self._broadcast("serial", snapshot)

    def update_peer(self, peer_id: int, payload: Dict[str, float]) -> None:
        with self._lock:
            record = self.peers.setdefault(peer_id, PeerRecord(peer_id=peer_id))
            record.mark_update(
                lat_deg=payload["lat"],
                lon_deg=payload["lon"],
                alt_cm=payload["alt_cm"],
                heading_deg=payload["heading"],
                speed_cms=payload["speed_cms"],
                link_quality=int(payload["lq"]),
                state=int(payload["state"]),
            )
            self.peer_updates += 1
            snapshot = self._peer_status_locked()
        self._broadcast("peers", snapshot)


    def _alias_for(self, raw_id: int) -> Tuple[str, str]:
        alias = self.peer_aliases.get(raw_id)
        if alias:
            label, name = alias
        else:
            label = PEER_SLOT_NAMES[raw_id] if raw_id < len(PEER_SLOT_NAMES) else f"{raw_id}"
            name = f"Peer {raw_id}"
        if not label:
            label = f"{raw_id}"
        if not name:
            name = label
        return label, name

    def _system_status_locked(self) -> Dict[str, object]:
        return {
            "target": "FormationFlight MSP Bridge",
            "platform": platform.system(),
            "version": BRIDGE_VERSION,
            "gitHash": "",
            "buildTime": "",
            "uptimeMilliseconds": ms_now() - self.start_ms,
            "name": self.device_name,
            "serial": {
                "connected": self.serial_connected,
                "port": self.serial_port,
                "baud": self.serial_baud,
            },
        }

    def _peer_status_locked(self) -> Dict[str, object]:
        now_ms = ms_now()
        peers_sorted = sorted(self.peers.values(), key=lambda p: p.peer_id)
        peers_json = []
        active_count = 0
        for peer in peers_sorted:
            if peer.peer_id <= 0:
                continue
            alias_id, alias_name = self._alias_for(peer.peer_id)
            peers_json.append(peer.json(None, now_ms, alias_id, alias_name))
            if now_ms - peer.last_update_ms <= LORA_PEER_TIMEOUT_MS:
                active_count += 1
        my_label, _ = self._alias_for(0)
        return {
            "myID": my_label,
            "count": len(peers_json),
            "countActive": active_count,
            "maxPeers": NODES_MAX,
            "peers": peers_json,
        }

    def _broadcast(self, event_type: str, payload: Dict[str, object]) -> None:
        if self._broadcaster is not None:
            self._broadcaster.publish({"type": event_type, "payload": payload})

    # Snapshot builders -------------------------------------------------
    def system_status(self) -> Dict[str, object]:
        with self._lock:
            return self._system_status_locked()

    def peer_status(self) -> Dict[str, object]:
        with self._lock:
            return self._peer_status_locked()

    def initial_messages(self) -> List[Dict[str, object]]:
        return [
            {"type": "serial", "payload": {
                "connected": self.serial_connected,
                "port": self.serial_port,
                "baud": self.serial_baud,
            }},
            {"type": "peers", "payload": self.peer_status()},
        ]


class MSPFrameDecoder:
    def __init__(self) -> None:
        self.buffer = bytearray()

    def feed(self, data: bytes) -> Iterable[Tuple[str, int, bytes]]:
        self.buffer.extend(data)
        frames: List[Tuple[str, int, bytes]] = []
        while True:
            frame = self._extract_frame()
            if frame is None:
                break
            frames.append(frame)
        return frames

    def _extract_frame(self) -> Optional[Tuple[str, int, bytes]]:
        buf = self.buffer
        while len(buf) >= 5:
            if buf[0] != 0x24:  # '$'
                del buf[0]
                continue
            if buf[1] == 0x4D and len(buf) >= 6:  # 'M'
                direction = buf[2]
                if direction not in (0x3C, 0x3E):  # '<' or '>'
                    del buf[0]
                    continue
                size = buf[3]
                total = 6 + size
                if len(buf) < total:
                    return None
                message_id = buf[4]
                payload = bytes(buf[5 : 5 + size])
                checksum = buf[5 + size]
                checksum_calc = size ^ message_id
                for b in payload:
                    checksum_calc ^= b
                del buf[:total]
                if checksum_calc == checksum:
                    return ("v1", message_id, payload)
                continue
            if buf[1] == 0x58 and len(buf) >= 9:  # 'X'
                direction = buf[2]
                if direction not in (0x3C, 0x3E):
                    del buf[0]
                    continue
                flags = buf[3]
                message_id = buf[4] | (buf[5] << 8)
                size = buf[6] | (buf[7] << 8)
                total = 9 + size
                if len(buf) < total:
                    return None
                payload = bytes(buf[8 : 8 + size])
                checksum = buf[8 + size]
                crc = 0
                for b in buf[3 : 8]:
                    crc = crc8_dvb_s2(crc, b)
                for b in payload:
                    crc = crc8_dvb_s2(crc, b)
                del buf[:total]
                if crc == checksum:
                    _ = flags
                    return ("v2", message_id, payload)
                continue
            del buf[0]
        return None


class SerialWorker(threading.Thread):
    def __init__(self, port: str, baud: int, state: BridgeState, stop_event: threading.Event) -> None:
        super().__init__(daemon=True)
        self.port = port
        self.baud = baud
        self.state = state
        self.stop_event = stop_event
        self.decoder = MSPFrameDecoder()
        self.serial: Optional[serial.Serial] = None

    def run(self) -> None:
        if serial is None:
            logging.error("pyserial is not installed")
            return
        while not self.stop_event.is_set():
            try:
                if self.serial is None or not self.serial.is_open:
                    self.serial = serial.Serial(self.port, self.baud, timeout=0.2)
                    logging.info("Opened serial port %s @ %d baud", self.port, self.baud)
                    self._send_msp_ident()
                data = self.serial.read(self.serial.in_waiting or 1)
                if data:
                    for version, message_id, payload in self.decoder.feed(data):
                        self._handle_frame(version, message_id, payload)
            except SerialException as exc:
                logging.warning("Serial error: %s", exc)
                self.state.set_serial_connection(False, None, self.baud)
                self._close_serial()
                time.sleep(1.0)
            except Exception as exc:
                logging.exception("Unexpected error while reading serial: %s", exc)
                time.sleep(0.5)
        self._close_serial()

    def _close_serial(self) -> None:
        if self.serial is not None:
            try:
                self.serial.close()
            except Exception:
                pass
        self.serial = None

    def _send_msp_ident(self) -> None:
        # Send MSP_FC_VARIANT to identify as GCS
        # Payload: "GCS" (null terminated) which is not a real value but doesn't break anything. Future fix for MSP.
        payload = b'GCS\x00'
        self._send_msp(MSP_FC_VARIANT, payload)

    def _send_msp(self, message_id: int, payload: bytes) -> None:
        if self.serial is None or not self.serial.is_open:
            return
        size = len(payload)
        checksum = size ^ message_id
        for b in payload:
            checksum ^= b
        data = b'$M<' + bytes([size, message_id]) + payload + bytes([checksum])
        self.serial.write(data)
        self.serial.flush()

    def _handle_frame(self, version: str, message_id: int, payload: bytes) -> None:
        logging.debug("Received MSP message: version=%s, id=%d, payload=%s", version, message_id, payload.hex())
        try:
            if version == "v1":
                self._handle_v1(message_id, payload)
            elif version == "v2":
                self._handle_v2(message_id, payload)
        except Exception as exc:
            logging.debug("Failed to parse MSP message %s:%d: %s", version, message_id, exc)

    def _handle_v1(self, message_id: int, payload: bytes) -> None:
        pass

    def _handle_v2(self, message_id: int, payload: bytes) -> None:
        if message_id == MSP2_COMMON_SET_RADAR_POS:
            self._parse_radar_pos(payload)

    def _parse_radar_pos(self, payload: bytes) -> None:
        if len(payload) < 19:
            return
        peer_id, state, lat_raw, lon_raw, alt_cm, heading, speed_cms, lq = struct.unpack(
            "<B B i i i H H B", payload[:19]
        )
        peer_payload = {
            "lat": lat_raw / 10_000_000.0,
            "lon": lon_raw / 10_000_000.0,
            "alt_cm": float(alt_cm),
            "heading": float(heading),
            "speed_cms": float(speed_cms),
            "lq": float(lq),
            "state": float(state),
        }
        self.state.update_peer(peer_id, peer_payload)


class WebSocketBroadcaster:
    def __init__(self, state: BridgeState, host: str, port: int) -> None:
        if websockets is None:
            raise RuntimeError("websockets package is required for streaming")
        self.state = state
        self.host = host
        self.port = port
        self.loop = asyncio.new_event_loop()
        self.queue: Optional[asyncio.Queue[Optional[Dict[str, object]]]] = None
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.clients: set = set()
        self._started = threading.Event()
        self._server = None

    def start(self) -> None:
        self.thread.start()
        self._started.wait()

    def publish(self, message: Dict[str, object]) -> None:
        if self.queue is None or not self.loop.is_running():
            return
        asyncio.run_coroutine_threadsafe(self.queue.put(message), self.loop)

    def shutdown(self) -> None:
        if self.queue is not None:
            asyncio.run_coroutine_threadsafe(self.queue.put(None), self.loop)
        self.loop.call_soon_threadsafe(self.loop.stop)
        self.thread.join(timeout=0.5)

    async def _start_server(self) -> None:
        assert websockets is not None
        self._server = await websockets.serve(self._handler, self.host, self.port)

    async def _stop_server(self) -> None:
        if self._server is None:
            return
        self._server.close()
        await self._server.wait_closed()
        self._server = None

    def _run(self) -> None:
        asyncio.set_event_loop(self.loop)
        self.queue = asyncio.Queue()
        self.loop.run_until_complete(self._start_server())
        self.loop.create_task(self._pump_queue())
        self._started.set()
        try:
            self.loop.run_forever()
        finally:
            self.loop.run_until_complete(self._stop_server())
            self.loop.run_until_complete(self._close_clients())

    async def _handler(self, websocket):
        self.clients.add(websocket)
        try:
            for message in self.state.initial_messages():
                await websocket.send(json.dumps(message))
            await websocket.wait_closed()
        finally:
            self.clients.discard(websocket)

    async def _pump_queue(self) -> None:
        assert self.queue is not None
        while True:
            message = await self.queue.get()
            if message is None:
                break
            if self.clients:
                data = json.dumps(message)
                assert websockets is not None
                websockets.broadcast(self.clients, data)

    async def _close_clients(self) -> None:
        if not self.clients:
            return
        await asyncio.gather(*(client.close() for client in list(self.clients)), return_exceptions=True)
        self.clients.clear()


class SerialManager:
    def __init__(self, state: BridgeState) -> None:
        self.state = state
        self._lock = threading.RLock()
        self.worker: Optional[SerialWorker] = None
        self.stop_event: Optional[threading.Event] = None
        self.port: Optional[str] = None
        self.baud: int = DEFAULT_SERIAL_BAUD

    def start(self, port: str, baud: int) -> None:
        with self._lock:
            self._stop_locked()
            stop_event = threading.Event()
            worker = SerialWorker(port, baud, self.state, stop_event)
            worker.start()
            self.worker = worker
            self.stop_event = stop_event
            self.port = port
            self.baud = baud
        self.state.set_serial_connection(True, port, baud)

    def stop(self) -> None:
        with self._lock:
            self._stop_locked()
        self.state.set_serial_connection(False, None, self.baud)

    def _stop_locked(self) -> None:
        if self.worker is not None and self.stop_event is not None:
            self.stop_event.set()
            self.worker.join(timeout=0.5)
        self.worker = None
        self.stop_event = None
        self.port = None

    def status(self) -> Dict[str, Union[bool, Optional[str], int]]:
        with self._lock:
            return {
                "connected": self.worker is not None,
                "port": self.port,
                "baud": self.baud,
            }


class BridgeController:
    def __init__(self, state: BridgeState, serial_manager: SerialManager, static_dir: Path) -> None:
        self.state = state
        self.serial_manager = serial_manager
        self.static_dir = static_dir


class BridgeRequestHandler(BaseHTTPRequestHandler):
    controller: BridgeController

    def do_OPTIONS(self) -> None:
        self.send_response(204)
        self._cors_headers()
        self.end_headers()

    def do_GET(self) -> None:
        parsed = urlsplit(self.path)
        path = parsed.path
        if path == "/system/status":
            self._send_json(self.controller.state.system_status())
        elif path == "/peermanager/status":
            self._send_json(self.controller.state.peer_status())
        elif path == "/api/status":
            self._send_json(self.controller.serial_manager.status())
        else:
            self._serve_static(path)

    def do_POST(self) -> None:
        parsed = urlsplit(self.path)
        path = parsed.path
        if path == "/api/connect":
            payload = self._read_json()
            port = payload.get("port") if isinstance(payload, dict) else None
            baud = payload.get("baud") if isinstance(payload, dict) else None
            if not port:
                self._send_json({"error": "port is required"}, status=400)
                return
            try:
                baud_int = int(baud) if baud else DEFAULT_SERIAL_BAUD
            except ValueError:
                self._send_json({"error": "invalid baud"}, status=400)
                return
            try:
                self.controller.serial_manager.start(port, baud_int)
            except Exception as exc:
                logging.exception("Failed to connect to %s", port)
                self._send_json({"error": str(exc)}, status=500)
                return
            self._send_json({"status": "connected", "port": port, "baud": baud_int})
        elif path == "/api/disconnect":
            self.controller.serial_manager.stop()
            self._send_json({"status": "disconnected"})
        else:
            self._send_json({"error": "not found"}, status=404)

    def log_message(self, format: str, *args: object) -> None:
        logging.info("HTTP %s - %s", self.address_string(), format % args)

    # Helpers
    def _serve_static(self, path: str) -> None:
        if path in ("", "/"):
            relative = Path("index.html")
        else:
            relative = Path(path.lstrip("/"))
        static_dir = self.controller.static_dir
        file_path = (static_dir / relative).resolve()
        try:
            file_path.relative_to(static_dir.resolve())
        except ValueError:
            self._send_json({"error": "not found"}, status=404)
            return
        if not file_path.exists() or not file_path.is_file():
            self._send_json({"error": "not found"}, status=404)
            return
        content_type, _ = mimetypes.guess_type(str(file_path))
        if content_type is None:
            content_type = "application/octet-stream"
        data = file_path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self._cors_headers()
        self.end_headers()
        self.wfile.write(data)

    def _read_json(self) -> Union[dict, list, str, int, float, None]:
        length = int(self.headers.get("Content-Length", 0))
        if length <= 0:
            return None
        body = self.rfile.read(length)
        try:
            return json.loads(body.decode("utf-8"))
        except json.JSONDecodeError:
            return None

    def _send_json(self, payload: Dict[str, Any], status: int = 200) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self._cors_headers()
        self.end_headers()
        self.wfile.write(body)

    def _cors_headers(self) -> None:
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")


def build_handler(controller: BridgeController):
    class Handler(BridgeRequestHandler):
        pass

    Handler.controller = controller
    return Handler


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="FormationFlight MSP telemetry bridge",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--http-port", type=int, default=DEFAULT_HTTP_PORT, help="HTTP server port")
    parser.add_argument("--ws-host", default=DEFAULT_WS_HOST, help="WebSocket bind address")
    parser.add_argument("--ws-port", type=int, default=DEFAULT_WS_PORT, help="WebSocket port")
    parser.add_argument("--device-name", default="MSP Bridge", help="Name exposed via /system/status")
    parser.add_argument("--log-level", default="INFO", choices=["DEBUG", "INFO", "WARNING", "ERROR"], help="Logging level")
    parser.add_argument("--no-websocket", action="store_true", help="Disable WebSocket streaming")
    parser.add_argument("--peer-map", action="append", default=[], metavar="RAWID=LABEL[:NAME]", help="Override peer slot labels/display names")
    parser.add_argument("--peer-map-file", type=Path, help="JSON file containing peer label/name overrides")
    parser.add_argument("--auto-connect", metavar="PORT", help="Serial port to connect automatically on startup")
    parser.add_argument("--baud", type=int, default=DEFAULT_SERIAL_BAUD, help="Serial baud for auto-connect")
    return parser.parse_args(argv)


def load_peer_aliases(args: argparse.Namespace) -> Dict[int, Tuple[str, str]]:
    aliases: Dict[int, Tuple[str, str]] = {}
    raw_entries: Dict[str, Union[str, Dict[str, str]]] = {}

    if args.peer_map_file:
        try:
            data = json.loads(args.peer_map_file.read_text())
            if isinstance(data, dict):
                raw_entries.update({str(k): v for k, v in data.items()})
            else:
                logging.error("Peer map file must contain a JSON object")
        except Exception as exc:
            logging.error("Failed to load peer map file %s: %s", args.peer_map_file, exc)

    for entry in args.peer_map:
        if "=" not in entry:
            logging.warning("Ignoring malformed --peer-map entry '%s'", entry)
            continue
        key, value = entry.split("=", 1)
        raw_entries[key.strip()] = value.strip()

    for key, value in raw_entries.items():
        try:
            raw_id = int(key)
        except ValueError:
            logging.warning("Peer map key '%s' is not an integer; skipping", key)
            continue

        label: str
        name: str

        if isinstance(value, str):
            if ":" in value:
                label_part, name_part = value.split(":", 1)
                label = label_part.strip()
                name = name_part.strip()
            else:
                label = value.strip()
                name = value.strip()
        elif isinstance(value, dict):
            label = str(value.get("id") or value.get("label") or value.get("name") or f"P{raw_id}")
            name = str(value.get("name") or value.get("label") or value.get("id") or label)
        else:
            logging.warning("Unsupported peer map value for key '%s'; skipping", key)
            continue

        label = label or f"P{raw_id}"
        name = name or label
        aliases[raw_id] = (label, name)

    if aliases:
        logging.info("Loaded %d peer alias overrides", len(aliases))
    return aliases


def main(argv: Optional[List[str]] = None) -> int:
    args = parse_args(argv)
    logging.basicConfig(
        level=getattr(logging, args.log_level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(message)s",
    )

    if serial is None:
        logging.error("pyserial is required: pip install pyserial")
        return 1

    static_dir = DEFAULT_STATIC_DIR.resolve()
    if not static_dir.exists():
        logging.error("Static directory %s does not exist", static_dir)
        return 1

    peer_aliases = load_peer_aliases(args)
    state = BridgeState(device_name=args.device_name, peer_aliases=peer_aliases)
    serial_manager = SerialManager(state)
    controller = BridgeController(state, serial_manager, static_dir)

    broadcaster: Optional[WebSocketBroadcaster] = None
    if args.no_websocket:
        logging.info("WebSocket streaming disabled")
    else:
        if websockets is None:
            logging.error("Install the 'websockets' package or use --no-websocket")
            return 1
        try:
            broadcaster = WebSocketBroadcaster(state, args.ws_host, args.ws_port)
            state.set_broadcaster(broadcaster)
            broadcaster.start()
            logging.info("WebSocket streaming available on ws://%s:%d/", args.ws_host, args.ws_port)
        except OSError as exc:
            logging.error("Failed to start WebSocket server: %s", exc)
            return 1

    server = ThreadingHTTPServer(("0.0.0.0", args.http_port), build_handler(controller))

    def shutdown_handler(signum: int, frame: object) -> None:
        logging.info("Received signal %s, shutting down", signum)
        server.shutdown()
        serial_manager.stop()
        if broadcaster:
            broadcaster.shutdown()

    signal.signal(signal.SIGTERM, shutdown_handler)

    if args.auto_connect:
        serial_manager.start(args.auto_connect, args.baud)

    logging.info(
        "Serving FormationFlight bridge UI on http://0.0.0.0:%d/", args.http_port
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        logging.info("Keyboard interrupt received, shutting down")
    finally:
        serial_manager.stop()
        if broadcaster is not None:
            broadcaster.shutdown()
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
