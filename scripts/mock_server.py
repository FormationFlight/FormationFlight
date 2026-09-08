#!/usr/bin/env python3
"""Mock FormationFlight device backend, for previewing/testing html/ in a
browser without real ESP32 hardware.

Serves html/ directly (no copying — edits to those files show up on the next
browser refresh, no rebuild step) and rewrites each response's
`const ENDPOINT_PREFIX = ...;` line on the fly to `""`, so relative fetches
in main.js/follow.js land on this same server instead of the hardcoded
192.168.4.1 device IP. Also serves fake JSON for the endpoints those files
call, including a stateful GET/POST /followmanager/config that round-trips
the same way FollowManager::applyConfig()/configJson() do on real firmware.

Usage:
    python3 scripts/mock_server.py [--port 8731]

Then open http://127.0.0.1:<port>/#/follow (or just / for the dashboard).
"""
import argparse
import copy
import json
import math
import re
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

START = time.time()

# Matches main.js/follow.js's `const ENDPOINT_PREFIX = window.location.host
# != "192.168.4.1" ? "http://192.168.4.1" : "";` line regardless of exact
# wording, so this keeps working if that line is ever reworded.
ENDPOINT_PREFIX_RE = re.compile(r"const ENDPOINT_PREFIX = .*?;")


def find_repo_root():
    d = Path(__file__).resolve().parent
    for _ in range(10):
        if (d / "platformio.ini").exists():
            return d
        if d.parent == d:
            break
        d = d.parent
    raise SystemExit("Couldn't find repo root (no platformio.ini above this script)")


REPO_ROOT = find_repo_root()
STATIC_DIR = REPO_ROOT / "html"

DEFAULT_CONFIG = {
    "ofsLongM": -15.0, "ofsLatM": 0.0, "ofsVertM": 10.0,
    "triggerMode": "GCSNAV",
    "targetPeer": 0, "emitHz": 4, "peerTimeoutMs": 1500,
    "minSepM": 8.0, "minVSepM": 13.0, "maxTargetDistM": 50.0, "minAltM": 3.0,
    "minCourseSpeed": 2.0,
    "headingMode": "POINT_LEADER", "headingDeg": 0.0,
    # Pre-populated (rather than -1/disabled) so the OSD Status panel has
    # something to show without needing to be configured first.
    "statusGvarIndex": 6, "conditionFlagsGvarIndex": 7,
    "rcLongChannel": -1, "rcLatChannel": -1, "rcVertChannel": -1,
    "targetSpeedGvarIndex": -1, "autothrottleEngageGvarIndex": -1,
    "autothrottleEnableRcChannel": -1,
    "autothrottleEnableMinThresholdUs": 1700, "autothrottleEnableMaxThresholdUs": 2100,
    "speedCorrectionAccelCmS2": 0, "minTargetSpeedMps": 0.0, "maxTargetSpeedMps": 0.0,
    "debug": False,
}
CONFIG = copy.deepcopy(DEFAULT_CONFIG)

# InavPlatformType (src/lib/MSP/MSP.h): 0=multirotor, 1=airplane, ... Flip
# here, or override per-request with ?platformType=N on GET
# /followmanager/status, to exercise the autothrottle panel's platform gate
# without a real FC — there's no real MSP connection in this mock.
MOCK_PLATFORM_TYPE = 1

PEERS = [
    {"rawId": 1, "id": "1", "name": "Falcon", "updated": 0, "age": 120, "lost": 0,
     "lat": 47.641, "lon": -122.140, "latRaw": 476410000, "lonRaw": -1221400000,
     "alt": 85, "groundSpeed": 850, "groundCourse": 900, "distance": 22.4,
     "courseTo": 88, "relativeAltitude": 4, "packetsReceived": 5231, "lq": 98},
    {"rawId": 2, "id": "2", "name": "Eagle", "updated": 0, "age": 340, "lost": 0,
     "lat": 47.642, "lon": -122.141, "latRaw": 476420000, "lonRaw": -1221410000,
     "alt": 90, "groundSpeed": 0, "groundCourse": 0, "distance": 5.1,
     "courseTo": 10, "relativeAltitude": 1, "packetsReceived": 2011, "lq": 91},
]


def system_status():
    return {
        "target": "DIY_ESPNOW", "platform": "ESP32",
        "version": "dev", "gitHash": "mockpreview", "buildTime": "now",
        "cloudBuild": False, "heap": 123456,
        "uptimeMilliseconds": int((time.time() - START) * 1000),
        "phase": 5, "name": "MOCK1", "longName": "mock-preview-craft",
        "host": "INAV", "state": 0,
    }


EARTH_RADIUS_M = 6371000
HEX_PATH_SIDES = 6
HEX_PATH_PEAK_ALT_M = 100.0


# Hand-maintained mirror of GNSSManager::calculatePointAtDistance() (src/lib/GNSS/
# GNSSManager.cpp) so the hexagon-patrol mock below traces the same path the firmware would.
def calculate_point_at_distance(lat, lon, distance_m, bearing_deg):
    lat1 = math.radians(lat)
    angular_distance = distance_m / EARTH_RADIUS_M
    bearing_rad = math.radians(bearing_deg)
    lat2 = math.asin(math.sin(lat1) * math.cos(angular_distance) +
                      math.cos(lat1) * math.sin(angular_distance) * math.cos(bearing_rad))
    lon2 = math.radians(lon) + math.atan2(
        math.sin(bearing_rad) * math.sin(angular_distance) * math.cos(lat1),
        math.cos(angular_distance) - math.sin(lat1) * math.sin(lat2))
    return math.degrees(lat2), math.degrees(lon2)


# Hand-maintained mirror of GNSSManager.cpp's distanceMeters()/courseDegrees() free functions.
def distance_meters(lat1, lon1, lat2, lon2):
    lat1r, lon1r, lat2r, lon2r = map(math.radians, (lat1, lon1, lat2, lon2))
    u = math.sin((lat2r - lat1r) / 2)
    v = math.sin((lon2r - lon1r) / 2)
    return 2.0 * EARTH_RADIUS_M * math.asin(math.sqrt(u * u + math.cos(lat1r) * math.cos(lat2r) * v * v))


def course_degrees(lat1, lon1, lat2, lon2):
    dlon = math.radians(lon2 - lon1)
    lat1r, lat2r = math.radians(lat1), math.radians(lat2)
    a1 = math.sin(dlon) * math.cos(lat2r)
    a2 = math.cos(lat1r) * math.sin(lat2r) - math.sin(lat1r) * math.cos(lat2r) * math.cos(dlon)
    bearing = math.atan2(a1, a2)
    if bearing < 0.0:
        bearing += 2 * math.pi
    return math.degrees(bearing)


# Hand-maintained mirror of PeerManager::spoofPeerHexPath()/updateHexPathPeer() (src/lib/Peers/
# PeerManager.cpp) for dev-only client testing of the "single aircraft, chase a moving fake
# peer" bench-test workflow — not a substitute for the firmware's own math.
class HexPath:
    def __init__(self, center_lat, center_lon, side_length, speed, start_alt):
        self.center_lat = center_lat
        self.center_lon = center_lon
        self.side_length = side_length
        self.speed = speed
        self.start_alt = start_alt
        self.leg_index = 0
        self.leg_progress = 0.0
        self.last_update = time.time()

    def advance(self):
        now = time.time()
        dt = now - self.last_update
        self.last_update = now
        self.leg_progress += self.speed * dt
        while self.side_length > 0 and self.leg_progress >= self.side_length:
            self.leg_progress -= self.side_length
            self.leg_index = (self.leg_index + 1) % HEX_PATH_SIDES

    def position(self):
        step_deg = 360.0 / HEX_PATH_SIDES
        leg_start_bearing = self.leg_index * step_deg
        course = (leg_start_bearing + step_deg / 2.0 + 90.0) % 360.0
        leg_start_lat, leg_start_lon = calculate_point_at_distance(
            self.center_lat, self.center_lon, self.side_length, leg_start_bearing)
        lat, lon = calculate_point_at_distance(leg_start_lat, leg_start_lon, self.leg_progress, course)

        # Linear "tent": start_alt at the loop's start/end vertex, up to the fixed peak
        # at the halfway vertex (after 3 of 6 edges), back down to start_alt at the close.
        total_progress = self.leg_index * self.side_length + self.leg_progress
        half_perimeter = 3.0 * self.side_length
        altitude = self.start_alt
        if half_perimeter > 0:
            if total_progress <= half_perimeter:
                altitude = self.start_alt + (HEX_PATH_PEAK_ALT_M - self.start_alt) * (total_progress / half_perimeter)
            else:
                fraction = (total_progress - half_perimeter) / half_perimeter
                altitude = HEX_PATH_PEAK_ALT_M + (self.start_alt - HEX_PATH_PEAK_ALT_M) * fraction

        return lat, lon, course, altitude


HEX_PATHS = {}  # peer index -> HexPath, for /peermanager/spoof(sideLength=...)


def peermanager_status():
    peers = [dict(p) for p in PEERS]
    for index, path in HEX_PATHS.items():
        if index >= len(peers):
            continue
        path.advance()
        lat, lon, course, altitude = path.position()
        peers[index]["lat"] = lat
        peers[index]["lon"] = lon
        peers[index]["latRaw"] = int(lat * 1e6)
        peers[index]["lonRaw"] = int(lon * 1e6)
        peers[index]["groundCourse"] = int(course * 10)
        peers[index]["groundSpeed"] = int(path.speed * 100)
        peers[index]["alt"] = int(altitude)

        g = gnssmanager_status()
        peers[index]["distance"] = distance_meters(g["lat"], g["lon"], lat, lon)
        peers[index]["courseTo"] = int(course_degrees(g["lat"], g["lon"], lat, lon))
        peers[index]["relativeAltitude"] = int(altitude - g["alt"])
    return {"myID": "0", "count": len(peers), "countActive": len(peers),
            "maxPeers": 8, "peers": peers}


def gnssmanager_status():
    return {"activeProvider": "GPS", "lat": 47.6415, "lon": -122.1405, "alt": 88,
            "groundSpeed": 420, "groundCourse": 900, "numSat": 14, "fixType": 3}


def cryptomanager_status():
    return {"keyString": "mockmockmockmockmockmockmockmock", "enabled": True}


def radiomanager_status():
    return {"radios": [
        {"status": "OK", "counters": "tx=1234 rx=1230", "enabled": True},
    ]}


def mspmanager_status():
    return {"peerUpdatesSent": 4213, "gnssUpdatesSent": 8899,
            "vbat": 16.4, "mahDrawn": 812, "amps": 9.3}


def followmanager_status(query=None):
    query = query or {}
    platform_type = int(query.get("platformType", [MOCK_PLATFORM_TYPE])[0])

    status_val = CONFIG["statusGvarIndex"]
    cond_val = CONFIG["conditionFlagsGvarIndex"]
    doc = {
        "state": "LOCKED", "gateActive": True,
        "lockedId": 1, "lockedName": "Falcon",
        "lastTarget": {"lat": 476410500, "lon": -1221405500, "altCm": 1450,
                        "headingDeg": 270,
                        "ageMs": int((time.time() - START) * 1000) % 900},
    }
    if status_val is not None and status_val >= 0:
        doc["statusGvarValue"] = 2  # LOCKED
    if cond_val is not None and cond_val >= 0:
        doc["conditionFlagsGvarValue"] = 0  # no altitude-floor clamp active
    doc["platformType"] = platform_type
    # Mirrors FollowManager::autothrottleArmed()'s "no FC connected" fallback
    # (this mock has no real RC channel data to read) — always armed.
    doc["autothrottleArmed"] = True
    # Mirrors FollowManager.cpp's airframe gate combined with the
    # arm switch above.
    autothrottle_engaged = platform_type == 1  # INAV_PLATFORM_AIRPLANE
    doc["autothrottleEngaged"] = autothrottle_engaged
    doc["targetSpeedCmS"] = PEERS[0]["groundSpeed"] if autothrottle_engaged else 0
    doc["liveOffset"] = {
        "longM": CONFIG["ofsLongM"], "latM": CONFIG["ofsLatM"], "vertM": CONFIG["ofsVertM"],
    }
    doc["rcSlotFrozen"] = False
    doc["rcPreArmCheckFailed"] = False
    # Mirrors FollowManager.cpp's havePreArmCandidateOffset gating loosely
    # (any RC axis assigned) for previewing the panel; real firmware also
    # requires the craft to be disarmed, which this mock doesn't model.
    if any(CONFIG[f] != -1 for f in ("rcLongChannel", "rcLatChannel", "rcVertChannel")):
        doc["preArmCandidateOffset"] = {
            "longM": CONFIG["ofsLongM"], "latM": CONFIG["ofsLatM"], "vertM": CONFIG["ofsVertM"],
        }
    return doc


# PeerManager.h's NODES_MAX (src/lib/Peers/PeerManager.h) — 0 = FIRST_ACTIVE,
# 1-NODES_MAX pin to a specific peer id.
NODES_MAX = 6

# FollowManager.cpp's offsetGeometrySane(), used by both loop()
# (runtime) and applyConfig() (config-validation time) on the firmware side —
# mirrored here so a geometrically-insane static offset is rejected the same
# way in the mock as on real firmware.
STACKED_HORIZONTAL_EPSILON_M = 0.5


# Hand-maintained mirror of FollowManager::applyConfig() (src/lib/Follow/
# FollowManager.cpp) for dev-only client testing — not a substitute for the
# firmware's own validation, just enough to exercise the UI's error paths.
def validate_config(cfg):
    if not (cfg.get("emitHz", 0) > 0):
        return "emitHz must be > 0"
    if not (cfg.get("peerTimeoutMs", 0) > 0):
        return "peerTimeoutMs must be > 0"
    if cfg.get("minSepM", 0) < 0 or cfg.get("minVSepM", 0) < 0 or cfg.get("minAltM", 0) < 0:
        return "minSepM/minVSepM/minAltM must be >= 0"
    if not (cfg.get("maxTargetDistM", 0) > 0):
        return "maxTargetDistM must be > 0"
    if cfg.get("minCourseSpeed", 0) < 0:
        return "minCourseSpeed must be >= 0"
    if cfg.get("targetPeer", 0) > NODES_MAX:
        return "targetPeer out of range"
    sgi = cfg.get("statusGvarIndex", -1)
    if sgi < -1 or sgi > 7:
        return "statusGvarIndex must be -1 (disabled) or 0-7"
    cgi = cfg.get("conditionFlagsGvarIndex", -1)
    if cgi < -1 or cgi > 7:
        return "conditionFlagsGvarIndex must be -1 (disabled) or 0-7"
    for f in ("rcLongChannel", "rcLatChannel", "rcVertChannel"):
        v = cfg.get(f, -1)
        if v != -1 and (v < 1 or v > 16):
            return f"{f} must be -1 (disabled) or 1-16"
    for f in ("targetSpeedGvarIndex", "autothrottleEngageGvarIndex"):
        v = cfg.get(f, -1)
        if v < -1 or v > 7:
            return f"{f} must be -1 (disabled) or 0-7"
    art = cfg.get("autothrottleEnableRcChannel", -1)
    if art != -1 and (art < 1 or art > 16):
        return "autothrottleEnableRcChannel must be -1 (disabled) or 1-16"

    gvar_fields = ["statusGvarIndex", "conditionFlagsGvarIndex",
                   "targetSpeedGvarIndex", "autothrottleEngageGvarIndex"]
    gvar_values = [cfg.get(f, -1) for f in gvar_fields if cfg.get(f, -1) != -1]
    if len(set(gvar_values)) != len(gvar_values):
        return "GVAR indices must be unique (or -1/disabled)"
    rc_fields = ["rcLongChannel", "rcLatChannel", "rcVertChannel"]
    rc_values = [cfg.get(f, -1) for f in rc_fields if cfg.get(f, -1) != -1]
    if len(set(rc_values)) != len(rc_values):
        return "rcLongChannel/rcLatChannel/rcVertChannel must be unique (or -1/disabled)"
    if art != -1 and art in rc_values:
        return "autothrottleEnableRcChannel must differ from the RC axis channels (or -1/disabled)"
    if cfg.get("autothrottleEnableMaxThresholdUs", 0) <= cfg.get("autothrottleEnableMinThresholdUs", 0):
        return "autothrottleEnableMaxThresholdUs must be > autothrottleEnableMinThresholdUs"

    if art != -1 and (cfg.get("minTargetSpeedMps", 0) <= 0 or cfg.get("maxTargetSpeedMps", 0) <= cfg.get("minTargetSpeedMps", 0)):
        return "minTargetSpeedMps must be > 0 and maxTargetSpeedMps must be > minTargetSpeedMps when autothrottleEnableRcChannel is set"
    if cfg.get("speedCorrectionAccelCmS2", 0) < 0:
        return "speedCorrectionAccelCmS2 must be >= 0"

    # Offset geometry rules, mirroring FollowManager.cpp's
    # offsetGeometrySane() so a config accepted here can't be geometrically
    # insane (minimum 3D separation, minimum vertical gap when stacked).
    long_m = cfg.get("ofsLongM", 0.0)
    lat_m = cfg.get("ofsLatM", 0.0)
    vert_m = cfg.get("ofsVertM", 0.0)
    horizontal_mag = math.sqrt(long_m * long_m + lat_m * lat_m)
    mag3d = math.sqrt(horizontal_mag * horizontal_mag + vert_m * vert_m)
    min_sep_m = cfg.get("minSepM", 0.0)
    if mag3d < min_sep_m:
        return "slot magnitude is below minSepM (minimum 3D separation)"
    min_vsep_m = cfg.get("minVSepM", 0.0)
    if horizontal_mag < STACKED_HORIZONTAL_EPSILON_M and abs(vert_m) < min_vsep_m:
        return "stacked slot's vertical offset is below minVSepM"

    return None


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass  # keep console quiet; errors still surface via server.log if redirected

    def _json(self, obj, status=200):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _text(self, text, status=200):
        body = text.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        if path == "/followmanager/status":
            self._json(followmanager_status(parse_qs(parsed.query)))
            return
        routes = {
            "/system/status": system_status,
            "/peermanager/status": peermanager_status,
            "/gnssmanager/status": gnssmanager_status,
            "/cryptomanager/status": cryptomanager_status,
            "/radiomanager/status": radiomanager_status,
            "/mspmanager/status": mspmanager_status,
            "/followmanager/config": lambda: CONFIG,
        }
        if path in routes:
            self._json(routes[path]())
            return
        self._serve_static(path)

    def do_POST(self):
        path = urlparse(self.path).path
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8") if length else ""
        params = {k: v[0] for k, v in parse_qs(body).items()}

        if path == "/followmanager/config":
            new_cfg = dict(CONFIG)
            float_fields = ["ofsLongM", "ofsLatM", "ofsVertM", "minSepM", "minVSepM",
                             "maxTargetDistM", "minAltM", "minCourseSpeed", "headingDeg",
                             "minTargetSpeedMps", "maxTargetSpeedMps"]
            int_fields = ["targetPeer", "emitHz", "peerTimeoutMs",
                          "statusGvarIndex", "conditionFlagsGvarIndex",
                          "rcLongChannel", "rcLatChannel", "rcVertChannel",
                          "targetSpeedGvarIndex", "autothrottleEngageGvarIndex",
                          "autothrottleEnableRcChannel",
                          "autothrottleEnableMinThresholdUs", "autothrottleEnableMaxThresholdUs",
                          "speedCorrectionAccelCmS2"]
            for f in float_fields:
                if f in params:
                    new_cfg[f] = float(params[f])
            for f in int_fields:
                if f in params:
                    new_cfg[f] = int(params[f])
            if "headingMode" in params:
                new_cfg["headingMode"] = params["headingMode"]
            if "debug" in params:
                new_cfg["debug"] = params["debug"] == "true"

            err = validate_config(new_cfg)
            if err:
                self._text(err, 400)
                return
            CONFIG.clear()
            CONFIG.update(new_cfg)
            self._json(CONFIG)
            return

        if path == "/peermanager/spoof":
            index = int(params.get("index", 0))
            if "sideLength" in params:
                side_length = float(params["sideLength"])
                speed = float(params.get("speed", 0))
                g = gnssmanager_status()
                if "lat" in params and "lon" in params:
                    lat, lon = float(params["lat"]), float(params["lon"])
                else:
                    lat, lon = g["lat"], g["lon"]
                HEX_PATHS[index] = HexPath(lat, lon, side_length, speed, g["alt"] + 10.0)
            else:
                HEX_PATHS.pop(index, None)
            self._text("OK", 200)
            return

        if path in ("/followmanager/commit", "/system/reboot"):
            self._text("OK", 200)
            return

        self._text("not found", 404)

    def _serve_static(self, path):
        if path == "/":
            path = "/index.html"
        # Resolve-and-check keeps requests confined to STATIC_DIR even if a
        # path contains ../ — this server only ever runs against trusted
        # local content, but there's no reason to skip the check.
        fs_path = (STATIC_DIR / path.lstrip("/")).resolve()
        if STATIC_DIR.resolve() not in fs_path.parents and fs_path != STATIC_DIR.resolve():
            self._text("not found", 404)
            return
        try:
            data = fs_path.read_bytes()
        except (FileNotFoundError, IsADirectoryError):
            self._text("not found", 404)
            return

        ctype = "application/octet-stream"
        if path.endswith(".html"):
            ctype = "text/html"
        elif path.endswith(".js"):
            ctype = "application/javascript"
            data = ENDPOINT_PREFIX_RE.sub('const ENDPOINT_PREFIX = "";', data.decode("utf-8")).encode("utf-8")
        elif path.endswith(".css"):
            ctype = "text/css"
        elif path.endswith(".svg"):
            ctype = "image/svg+xml"
        elif path.endswith(".png"):
            ctype = "image/png"

        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8731)
    args = parser.parse_args()

    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"Serving {STATIC_DIR} — open http://127.0.0.1:{args.port}/#/follow (Ctrl+C to stop)")
    server.serve_forever()
