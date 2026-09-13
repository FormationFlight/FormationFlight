#!/usr/bin/env python3
"""Mock FormationFlight v2 device backend, for developing and testing html/ in a
browser with no ESP32 hardware attached.

Implements every endpoint in docs/v2-web-api.md against an in-memory node that
actually moves: peers fly real paths, per-radio counters climb, the frame log
rolls (with the occasional crypto_fail so the debug view's error styling has
something to render), and Follow walks IDLE -> ACQUIRING -> LOCKED the way the
controller does on hardware.

Several things here are hand-maintained mirrors of firmware code and must not
drift. Each carries a comment naming what it mirrors:

  - default_config() mirrors the struct defaults in lib/ff_core/config.h
    (via RateConfig in rate_control.h and FollowConfig in follow.h).
  - validate_config() mirrors ff::configValidate() in lib/ff_core/config.cpp and
    validate_follow_config() mirrors ff::followValidateConfig() in follow.cpp,
    down to the error strings, which the UI shows verbatim.
  - merge_config() mirrors ff::configMergeJson(), including the redaction
    placeholder rule and "a rejected merge changes nothing".
  - MovingPeer mirrors ff::SimPeerConfig and ff::simPeerAt() in sim_traffic.cpp.
  - The log ring (log_add()/log_json()/clear_log()) mirrors ff::LogRing in
    lib/ff_core/log.cpp, including the two rules that matter to a polling
    client: `total` counts everything ever logged and a clear does not reset it.
  - loop_json() mirrors ff::LoopStats (loop_stats.h), down to minUs() reading 0
    before the first sample and rateHz() being derived from the mean.
  - system_json() mirrors fillSystem() in WebServer.cpp.
  - The handlers' status codes and response bodies mirror src/hal/WebServer.cpp,
    which is the other implementation of this same contract. Where that file is
    more specific than the doc, it wins: it is what the UI will actually meet.

test/test_mock_server.py checks the Follow half against
docs/spec/fixtures/follow-config-cases.json -- the same fixture the C++
(test/test_follow/test_cross_mirror_fixture.cpp) and JS
(test/follow-logic.test.js) validators are tested against.

Static files are served straight out of html/ (no copy step: edit, refresh) with
each .js response's `const ENDPOINT_PREFIX = ...;` line rewritten to `""` so the
UI's fetches land on this server instead of the hardcoded 192.168.4.1 device IP.

Usage:
    python3 scripts/mock_server.py [--port 8731] [--sim] [--seed N]

Then open http://127.0.0.1:<port>/.
"""
import argparse
import copy
import json
import math
import random
import re
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

# ---------------------------------------------------------------------------
# Paths / static serving
# ---------------------------------------------------------------------------

# Matches the UI's `const ENDPOINT_PREFIX = window.location.host != "192.168.4.1"
# ? "http://192.168.4.1" : "";` line regardless of exact wording, so this keeps
# working if that line is ever reworded.
ENDPOINT_PREFIX_RE = re.compile(r"const ENDPOINT_PREFIX = .*?;")

CONTENT_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".js": "application/javascript",
    ".mjs": "application/javascript",
    ".css": "text/css",
    ".json": "application/json",
    ".map": "application/json",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".jpg": "image/jpeg",
    ".jpeg": "image/jpeg",
    ".gif": "image/gif",
    ".ico": "image/x-icon",
    ".woff": "font/woff",
    ".woff2": "font/woff2",
    ".txt": "text/plain; charset=utf-8",
}


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

# ---------------------------------------------------------------------------
# Firmware constants, mirrored
# ---------------------------------------------------------------------------

CONFIG_VERSION = 1                  # ff::kConfigVersion (config.h)
MAX_NAME_LEN = 15                   # ff::kMaxNameLen (protocol.h)
MAX_PASSPHRASE_LEN = 32             # ff::kMaxPassphraseLen (config.h)
MAX_SSID_LEN = 32                   # ff::kMaxSsidLen (config.h)
MAX_PSK_LEN = 63                    # ff::kMaxPskLen (config.h)
REDACTED_SECRET = "•" * 8      # ff::kRedactedSecret ("••••••••")
CRYPTO_DISABLED_PASSPHRASE = "none"  # ff::kCryptoDisabledPassphrase

MSP_MAX_RC_CHANNELS = 16            # ff::kMspMaxRcChannels (msp_fc.h)
GVAR_INDEX_MAX = 7
FRAME_LOG_CAPACITY = 32             # ff::kFrameLogCapacity (frame_log.h)
LOG_CAPACITY = 48                   # ff::kLogCapacity (log.h)
LOG_TEXT_LEN = 72                   # ff::kLogTextLen (log.h)
STACKED_HORIZONTAL_EPSILON_M = 0.5  # ff::kFollowStackedHorizontalEpsilonM (follow.h)

# ff::FrameResult (frame_log.h), in enum order; the API spells them lower-snake.
FRAME_RESULTS = ("tx", "ok", "self", "crypto_fail", "replay_fail", "decode_fail", "oversize")

# ff::LogLevel (log.h), in enum order; logLevelName() spells them like this.
LOG_LEVELS = ("debug", "info", "warn", "error")

# ff::FollowLockState (follow.h) -> followLockStateName()
FOLLOW_STATE_NAMES = ("IDLE", "ACQUIRING", "LOCKED", "LOCKED_HOLDING")
# ff::FollowHeadingMode (follow.h) -> followHeadingModeName()
HEADING_MODE_NAMES = ("OFF", "COURSE", "POINT_LEADER", "FIXED", "COURSE_RELATIVE")
# ff::FollowTriggerMode (follow.h) -> followTriggerModeName()
TRIGGER_MODE_NAMES = ("GCSNAV", "AUX")

# ff::PositionFlags (protocol.h)
POSITION_FLAG_ARMED = 1 << 0
POSITION_FLAG_HAS_FIX = 1 << 1

# Frame sizes on the air (protocol.h).
POSITION_FRAME_SIZE = 31
ANNOUNCE_FRAME_SIZE = 36

EARTH_RADIUS_M = 6371000.0

# ---------------------------------------------------------------------------
# Config: defaults, serialization, merge, validation
# ---------------------------------------------------------------------------

# Field name -> (kind, width, signed). Kinds: "i" integer (narrowed the way the
# C++ struct's fixed-width member would narrow it), "f" double, "b" bool.
_FOLLOW_FIELD_TYPES = {
    # FOLLOW_CONFIG_DIRECT_FIELDS
    # Carried on the wire as an 8-char hex string, like every other UID in the
    # API; stored here as an int, matching the C++ struct member.
    "targetUid": ("u", 32, False),
    "emitHz": ("i", 16, False),
    "peerTimeoutMs": ("i", 32, False),
    "statusGvarIndex": ("i", 16, True),
    "conditionFlagsGvarIndex": ("i", 16, True),
    "rcLongChannel": ("i", 16, True),
    "rcLatChannel": ("i", 16, True),
    "rcVertChannel": ("i", 16, True),
    "targetSpeedGvarIndex": ("i", 16, True),
    "autothrottleEngageGvarIndex": ("i", 16, True),
    "autothrottleEnableRcChannel": ("i", 16, True),
    "autothrottleEnableMinThresholdUs": ("i", 16, True),
    "autothrottleEnableMaxThresholdUs": ("i", 16, True),
    "speedCorrectionAccelCmS2": ("i", 16, True),
    # FOLLOW_CONFIG_ROUNDED_FIELDS (doubles in RAM; only the EEPROM record rounds)
    "ofsLongM": ("f", 0, True),
    "ofsLatM": ("f", 0, True),
    "ofsVertM": ("f", 0, True),
    "minSepM": ("f", 0, True),
    "minVSepM": ("f", 0, True),
    "maxTargetDistM": ("f", 0, True),
    "minAltM": ("f", 0, True),
    "minCourseSpeed": ("f", 0, True),
    "headingDeg": ("f", 0, True),
    "minTargetSpeedMps": ("f", 0, True),
    "maxTargetSpeedMps": ("f", 0, True),
    "debug": ("b", 0, False),
}


def default_follow_config():
    """ff::FollowConfig's member initialisers (lib/ff_core/follow.h)."""
    return {
        # DIRECT
        "targetUid": 0,
        "emitHz": 4,
        "peerTimeoutMs": 1500,
        "statusGvarIndex": -1,
        "conditionFlagsGvarIndex": -1,
        "rcLongChannel": -1,
        "rcLatChannel": -1,
        "rcVertChannel": -1,
        "targetSpeedGvarIndex": -1,
        "autothrottleEngageGvarIndex": -1,
        "autothrottleEnableRcChannel": -1,
        "autothrottleEnableMinThresholdUs": 1700,
        "autothrottleEnableMaxThresholdUs": 2100,
        "speedCorrectionAccelCmS2": 0,
        # ROUNDED
        "ofsLongM": -15.0,
        "ofsLatM": 0.0,
        "ofsVertM": 10.0,
        "minSepM": 8.0,
        "minVSepM": 13.0,
        "maxTargetDistM": 50.0,
        "minAltM": 3.0,
        "minCourseSpeed": 2.0,
        "headingDeg": 0.0,
        "minTargetSpeedMps": 0.0,
        "maxTargetSpeedMps": 0.0,
        # enums / RAM-only
        "headingMode": HEADING_MODE_NAMES[2],   # FOLLOW_HEADING_MODE, POINT_LEADER
        "triggerMode": TRIGGER_MODE_NAMES[0],   # FOLLOW_TRIGGER_MODE (GCSNAV),
                                                # compile-time, read-only in JSON
        "debug": False,                         # FOLLOW_DEBUG_ENABLED, never persisted
    }


def default_config():
    """ff::Settings' member initialisers (lib/ff_core/config.h), as the JSON
    document configToJson() produces."""
    return {
        "version": CONFIG_VERSION,
        "node": {"name": "", "listen_only": False},
        "security": {"passphrase": ""},
        # ff::RateConfig (rate_control.h)
        "rate": {
            "target_load": 0.15,
            "min_interval_ms": 100,
            "max_interval_ms": 1000,
            "jitter_frac": 0.25,
            # Regional duty-cycle ceiling, 0 = none. Seeded per band at build
            # time on real firmware (LORA_DUTY_PCT); 0 on the host build.
            "duty_cycle_pct": 0,
        },
        "peers": {"timeout_ms": 6000, "announce_interval_ms": 2000},
        "msp": {"radar_interval_ms": 100},
        "gnss": {"rate_hz": 10},
        "radios": {"espnow_enabled": True, "lora_enabled": True, "lora_power_dbm": 0},
        "wifi": {"ap": True, "ssid": "", "psk": "", "ap_psk": "", "channel": 1},
        "sim": {"enabled": False},
        "follow": default_follow_config(),
    }


# The three fields configToJson() runs through putSecret().
SECRET_FIELDS = (("security", "passphrase"), ("wifi", "psk"), ("wifi", "ap_psk"))


def config_to_json(cfg):
    """configToJson(): the wire form of the config. Fields stored as native
    integers that travel as strings are converted here, in one place."""
    out = copy.deepcopy(cfg)
    out["follow"]["targetUid"] = "%08x" % (out["follow"].get("targetUid", 0) & 0xFFFFFFFF)
    return out


def redact_config(cfg):
    """configToJson(..., redact_secrets=true): a set secret becomes the
    placeholder, an unset one stays the empty string."""
    out = config_to_json(cfg)
    for section, key in SECRET_FIELDS:
        if out.get(section, {}).get(key):
            out[section][key] = REDACTED_SECRET
    return out


def _narrow(value, bits, signed):
    value &= (1 << bits) - 1
    if signed and value >= (1 << (bits - 1)):
        value -= 1 << bits
    return value


def _as_int(v, bits=32, signed=True):
    """ArduinoJson's as<intN_t>(): numbers truncate toward zero and wrap into the
    destination width, anything else reads as 0."""
    if isinstance(v, bool):
        raw = int(v)
    elif isinstance(v, int):
        raw = v
    elif isinstance(v, float):
        raw = 0 if (math.isnan(v) or math.isinf(v)) else int(v)
    else:
        return 0
    return _narrow(raw, bits, signed)


def _as_float(v):
    if isinstance(v, bool):
        return float(v)
    if isinstance(v, (int, float)):
        return float(v)
    return 0.0


def _as_bool(v):
    if isinstance(v, bool):
        return v
    if isinstance(v, (int, float)):
        return v != 0
    return False


def _merge_scalar(src, key, dst, kind, bits=32, signed=True):
    """ff::mergeVal(): read the key only if it is present, leave dst alone
    otherwise. That is the whole merge contract."""
    if key not in src:
        return
    v = src[key]
    if kind == "u":
        # configMergeJson() takes a hex string, and still accepts a bare number
        # so a hand-written config file is easy to get right.
        if isinstance(v, str):
            try:
                dst[key] = int(v, 16) & 0xFFFFFFFF
            except ValueError:
                pass
        else:
            dst[key] = _as_int(v, 32, False)
    elif kind == "i":
        dst[key] = _as_int(v, bits, signed)
    elif kind == "f":
        dst[key] = _as_float(v)
    else:
        dst[key] = _as_bool(v)


def _copy_str(src, key, dst, cap):
    """ff::copyStr(): strings truncate rather than overflow, non-strings are
    ignored, and the redaction placeholder means 'leave this one alone'."""
    if key not in src:
        return
    v = src[key]
    if not isinstance(v, str):
        return
    if v == REDACTED_SECRET:
        return
    dst[key] = v[:cap]


def validate_follow_config(f):
    """Mirror of ff::followValidateConfig() (lib/ff_core/follow.cpp). Returns the
    error string, or None when the Follow block is valid."""
    def i(key, default=-1):
        return f.get(key, default)

    def d(key, default=0.0):
        return f.get(key, default)

    if not i("emitHz", 0) > 0:
        return "emitHz must be > 0"
    if not i("peerTimeoutMs", 0) > 0:
        return "peerTimeoutMs must be > 0"
    if d("minSepM") < 0 or d("minVSepM") < 0 or d("minAltM") < 0:
        return "minSepM/minVSepM/minAltM must be >= 0"
    if not d("maxTargetDistM") > 0:
        return "maxTargetDistM must be > 0"
    if d("minCourseSpeed") < 0:
        return "minCourseSpeed must be >= 0"
    # targetUid: any value is a valid UID (0 = first active); no range rule.
    if i("statusGvarIndex") < -1 or i("statusGvarIndex") > GVAR_INDEX_MAX:
        return "statusGvarIndex must be -1 (disabled) or 0-7"
    if i("conditionFlagsGvarIndex") < -1 or i("conditionFlagsGvarIndex") > GVAR_INDEX_MAX:
        return "conditionFlagsGvarIndex must be -1 (disabled) or 0-7"
    for key in ("rcLongChannel", "rcLatChannel", "rcVertChannel"):
        v = i(key)
        if v != -1 and (v < 1 or v > MSP_MAX_RC_CHANNELS):
            return f"{key} must be -1 (disabled) or 1-{MSP_MAX_RC_CHANNELS}"
    if i("targetSpeedGvarIndex") < -1 or i("targetSpeedGvarIndex") > GVAR_INDEX_MAX:
        return "targetSpeedGvarIndex must be -1 (disabled) or 0-7"
    if i("autothrottleEngageGvarIndex") < -1 or i("autothrottleEngageGvarIndex") > GVAR_INDEX_MAX:
        return "autothrottleEngageGvarIndex must be -1 (disabled) or 0-7"
    arm_ch = i("autothrottleEnableRcChannel")
    if arm_ch != -1 and (arm_ch < 1 or arm_ch > MSP_MAX_RC_CHANNELS):
        return f"autothrottleEnableRcChannel must be -1 (disabled) or 1-{MSP_MAX_RC_CHANNELS}"

    # Overlap rules: enforced here as well as in the web validator so a raw REST
    # client gets the same guarantee.
    assigned_gvars = [i(k) for k in ("statusGvarIndex", "conditionFlagsGvarIndex",
                                     "targetSpeedGvarIndex", "autothrottleEngageGvarIndex")
                      if i(k) != -1]
    if len(set(assigned_gvars)) != len(assigned_gvars):
        return "GVAR indices must be unique (or -1/disabled)"
    assigned_rc = [i(k) for k in ("rcLongChannel", "rcLatChannel", "rcVertChannel")
                   if i(k) != -1]
    if len(set(assigned_rc)) != len(assigned_rc):
        return "rcLongChannel/rcLatChannel/rcVertChannel must be unique (or -1/disabled)"
    if arm_ch != -1 and arm_ch in assigned_rc:
        return ("autothrottleEnableRcChannel must differ from the RC axis channels "
                "(or -1/disabled)")
    if i("autothrottleEnableMaxThresholdUs", 0) <= i("autothrottleEnableMinThresholdUs", 0):
        return "autothrottleEnableMaxThresholdUs must be > autothrottleEnableMinThresholdUs"
    # Only matters once a pilot has wired up an arm channel, so the compiled-in
    # 0/0 defaults can sit un-configured until then.
    if arm_ch != -1 and (d("minTargetSpeedMps") <= 0 or
                         d("maxTargetSpeedMps") <= d("minTargetSpeedMps")):
        return ("minTargetSpeedMps must be > 0 and maxTargetSpeedMps must be > "
                "minTargetSpeedMps when autothrottleEnableRcChannel is set")
    if i("speedCorrectionAccelCmS2", 0) < 0:
        # A magnitude fed through copysignf(); negative would push the wrong way.
        return "speedCorrectionAccelCmS2 must be >= 0"

    # Offset geometry rules against the canonical offset -- mirrors service()'s
    # check so an accepted config can never be rejected by it later.
    return offset_geometry_error(d("ofsLongM"), d("ofsLatM"), d("ofsVertM"),
                                 d("minSepM"), d("minVSepM"))


def offset_geometry_error(long_m, lat_m, vert_m, min_sep_m, min_vsep_m):
    """Mirror of ff::offsetGeometrySane() (lib/ff_core/follow.cpp)."""
    horizontal_mag = math.sqrt(long_m * long_m + lat_m * lat_m)
    mag3d = math.sqrt(horizontal_mag * horizontal_mag + vert_m * vert_m)
    # Minimum 3D separation -- forbids the degenerate collision slot.
    if mag3d < min_sep_m:
        return "slot magnitude is below minSepM (minimum 3D separation)"
    # Minimum vertical gap for stacked (overhead/underneath) slots -- absorbs GPS
    # vertical error, not just physical clearance.
    if horizontal_mag < STACKED_HORIZONTAL_EPSILON_M and abs(vert_m) < min_vsep_m:
        return "stacked slot's vertical offset is below minVSepM"
    return None


def validate_config(cfg):
    """Mirror of ff::configValidate() (lib/ff_core/config.cpp), including its
    rule order and its error strings. Returns the error, or None if valid."""
    rate = cfg.get("rate", {})
    peers = cfg.get("peers", {})
    msp = cfg.get("msp", {})
    gnss = cfg.get("gnss", {})
    radios = cfg.get("radios", {})
    wifi = cfg.get("wifi", {})
    sim = cfg.get("sim", {})

    if rate.get("target_load", 0.0) <= 0.0 or rate.get("target_load", 0.0) > 1.0:
        return "rate.target_load must be in (0, 1]"
    if rate.get("jitter_frac", 0.0) < 0.0 or rate.get("jitter_frac", 0.0) >= 1.0:
        return "rate.jitter_frac must be in [0, 1)"
    if rate.get("min_interval_ms", 0) == 0:
        return "rate.min_interval_ms must be > 0"
    if rate.get("max_interval_ms", 0) < rate.get("min_interval_ms", 0):
        return "rate.max_interval_ms must be >= rate.min_interval_ms"
    duty = rate.get("duty_cycle_pct", 0)
    if duty > 100:
        return "rate.duty_cycle_pct must be 0 (no limit) or 1-100"
    if peers.get("timeout_ms", 0) == 0:
        return "peers.timeout_ms must be > 0"
    if peers.get("announce_interval_ms", 0) == 0:
        return "peers.announce_interval_ms must be > 0"
    if msp.get("radar_interval_ms", 0) == 0:
        return "msp.radar_interval_ms must be > 0"
    if gnss.get("rate_hz", 0) == 0 or gnss.get("rate_hz", 0) > 25:
        return "gnss.rate_hz must be 1-25"
    # A node with no radio enabled is almost certainly a mistake, and it looks
    # identical to a hardware fault from the outside. Refuse it.
    if not radios.get("espnow_enabled") and not radios.get("lora_enabled") \
            and not sim.get("enabled"):
        return "at least one radio must be enabled"
    if radios.get("lora_power_dbm", 0) < 0 or radios.get("lora_power_dbm", 0) > 30:
        return "radios.lora_power_dbm must be 0 (target default) or 1-30"
    # The AP's channel, and therefore ESP-NOW's: the two share one radio. 14 is
    # Japan-only and the ESP refuses it, so the range stops at 13.
    if wifi.get("channel", 1) < 1 or wifi.get("channel", 1) > 13:
        return "wifi.channel must be 1-13"
    if not wifi.get("ap") and not wifi.get("ssid"):
        return "wifi.ssid is required when wifi.ap is false"
    # WPA2 will not accept a shorter key, and an AP that silently comes up open
    # because the password was too short is a nasty surprise.
    if wifi.get("ap_psk") and len(wifi["ap_psk"]) < 8:
        return "wifi.ap_psk must be empty (open) or at least 8 characters"

    # Follow's own rules, so a config accepted here can never be rejected by the
    # controller later.
    return validate_follow_config(cfg.get("follow", {}))


def merge_config(incoming, cfg):
    """Mirror of ff::configMergeJson(): applies only the keys present in
    `incoming` on top of a copy of `cfg`, then validates the result. Returns
    (merged, None) on success or (None, error) on failure -- on failure the
    caller's config must be left exactly as it was.

    `version` is deliberately not mergeable, same as the firmware: it is stamped
    by the build, not chosen by a client.
    """
    if not isinstance(incoming, dict):
        return None, "body must be a JSON object"
    nxt = copy.deepcopy(cfg)

    def section(name):
        v = incoming.get(name)
        return v if isinstance(v, dict) else None

    s = section("node")
    if s is not None:
        _copy_str(s, "name", nxt["node"], MAX_NAME_LEN)
        _merge_scalar(s, "listen_only", nxt["node"], "b")
    s = section("security")
    if s is not None:
        _copy_str(s, "passphrase", nxt["security"], MAX_PASSPHRASE_LEN)
    s = section("rate")
    if s is not None:
        _merge_scalar(s, "target_load", nxt["rate"], "f")
        _merge_scalar(s, "min_interval_ms", nxt["rate"], "i", 32, False)
        _merge_scalar(s, "max_interval_ms", nxt["rate"], "i", 32, False)
        _merge_scalar(s, "jitter_frac", nxt["rate"], "f")
        _merge_scalar(s, "duty_cycle_pct", nxt["rate"], "i", 8, False)
    s = section("peers")
    if s is not None:
        _merge_scalar(s, "timeout_ms", nxt["peers"], "i", 32, False)
        _merge_scalar(s, "announce_interval_ms", nxt["peers"], "i", 32, False)
    s = section("msp")
    if s is not None:
        _merge_scalar(s, "radar_interval_ms", nxt["msp"], "i", 32, False)
    s = section("gnss")
    if s is not None:
        _merge_scalar(s, "rate_hz", nxt["gnss"], "i", 16, False)
    s = section("radios")
    if s is not None:
        _merge_scalar(s, "espnow_enabled", nxt["radios"], "b")
        _merge_scalar(s, "lora_enabled", nxt["radios"], "b")
        _merge_scalar(s, "lora_power_dbm", nxt["radios"], "i", 16, True)
    s = section("wifi")
    if s is not None:
        _merge_scalar(s, "ap", nxt["wifi"], "b")
        _copy_str(s, "ssid", nxt["wifi"], MAX_SSID_LEN)
        _copy_str(s, "psk", nxt["wifi"], MAX_PSK_LEN)
        _copy_str(s, "ap_psk", nxt["wifi"], MAX_PSK_LEN)
        _merge_scalar(s, "channel", nxt["wifi"], "i", 8, False)
    s = section("sim")
    if s is not None:
        _merge_scalar(s, "enabled", nxt["sim"], "b")
    s = section("follow")
    if s is not None:
        f = nxt["follow"]
        for key, (kind, bits, signed) in _FOLLOW_FIELD_TYPES.items():
            _merge_scalar(s, key, f, kind, bits, signed)
        # headingMode is an enum by name; an unrecognised name is ignored, same
        # as mergeFollow(). triggerMode is compile-time, so it is never merged.
        if isinstance(s.get("headingMode"), str) and s["headingMode"] in HEADING_MODE_NAMES:
            f["headingMode"] = s["headingMode"]

    err = validate_config(nxt)
    if err:
        return None, err
    return nxt, None


# ---------------------------------------------------------------------------
# Geodesy (mirrors lib/ff_core/geo.cpp closely enough for a mock)
# ---------------------------------------------------------------------------

def point_at_distance(lat, lon, distance_m, bearing_deg):
    lat1 = math.radians(lat)
    ang = distance_m / EARTH_RADIUS_M
    brg = math.radians(bearing_deg)
    lat2 = math.asin(math.sin(lat1) * math.cos(ang) +
                     math.cos(lat1) * math.sin(ang) * math.cos(brg))
    lon2 = math.radians(lon) + math.atan2(
        math.sin(brg) * math.sin(ang) * math.cos(lat1),
        math.cos(ang) - math.sin(lat1) * math.sin(lat2))
    return math.degrees(lat2), math.degrees(lon2)


def distance_meters(lat1, lon1, lat2, lon2):
    lat1r, lon1r, lat2r, lon2r = map(math.radians, (lat1, lon1, lat2, lon2))
    u = math.sin((lat2r - lat1r) / 2)
    v = math.sin((lon2r - lon1r) / 2)
    return 2.0 * EARTH_RADIUS_M * math.asin(
        math.sqrt(u * u + math.cos(lat1r) * math.cos(lat2r) * v * v))


def bearing_degrees(lat1, lon1, lat2, lon2):
    dlon = math.radians(lon2 - lon1)
    lat1r, lat2r = math.radians(lat1), math.radians(lat2)
    y = math.sin(dlon) * math.cos(lat2r)
    x = math.cos(lat1r) * math.sin(lat2r) - math.sin(lat1r) * math.cos(lat2r) * math.cos(dlon)
    b = math.degrees(math.atan2(y, x))
    return b + 360.0 if b < 0 else b


def to_1e7(deg):
    return int(round(deg * 1e7))


def uid_str(uid):
    """API convention: lower-case hex, 8 characters, no prefix."""
    return f"{uid & 0xFFFFFFFF:08x}"


def parse_uid(text):
    """Parses an 8-char lower-case hex UID. Returns None if it is not one."""
    if not isinstance(text, str) or len(text) != 8:
        return None
    try:
        return int(text, 16)
    except ValueError:
        return None


# ---------------------------------------------------------------------------
# Motion models -- the four `mode` values docs/v2-web-api.md defines
# ---------------------------------------------------------------------------

SIM_MODES = ("static", "line", "circle", "hex")   # ff::SimMode (sim_traffic.h)
HEX_SIDES = 6
SIM_HEX_CLIMB_M = 80.0                            # ff::kSimHexClimbM
SIM_MAX_PEERS = 4                                 # ff::SimRadio::kMaxSimPeers
SIM_MAX_SPEED_MS = 200.0                          # WebServer.cpp's speed_ms bound


def course_to_ddeg(deg):
    """ff::courseToDdeg() (sim_traffic.cpp): wrap, scale, and fold a rounded
    360.0 back to 0 rather than letting it become 3600."""
    d = math.fmod(deg, 360.0)
    if d < 0.0:
        d += 360.0
    ddeg = int(round(d * 10.0))
    return 0 if ddeg >= 3600 else ddeg


class MovingPeer:
    """One aircraft flying a path -- the Python mirror of ff::SimPeerConfig plus
    ff::simPeerAt() (lib/ff_core/sim_traffic.cpp).

    Used both for the mock's built-in "RF" peers and for peers created through
    /api/sim/peer. On the firmware the simulator encodes these positions into
    real protocol packets and pushes them through the real receive path, so
    downstream the two are the same thing; here they are the same object for the
    same reason.
    """

    def __init__(self, uid, name, mode, lat, lon, alt_m, speed_ms, course_deg,
                 radius_m, radios=(0,), armed=False, sim=False, start_ms=0):
        self.uid = uid & 0xFFFFFFFF
        self.name = (name or "SIM")[:MAX_NAME_LEN]
        self.mode = mode if mode in SIM_MODES else "static"
        self.lat = float(lat)
        self.lon = float(lon)
        self.alt_m = int(round(float(alt_m)))   # int16_t metres MSL
        self.speed_ms = float(speed_ms)
        self.course_deg = float(course_deg)
        self.radius_m = float(radius_m)
        self.radios = list(radios)
        self.armed = bool(armed)
        self.sim = bool(sim)
        # The path starts at the moment the peer is created (SimRadio::setPeer).
        self.start_ms = int(start_ms)
        # Live counters, advanced by MockNode as frames are generated.
        self.packets = 0
        self.last_rx_ms = 0
        self.rssi = -70

    # -- path ---------------------------------------------------------------

    def elapsed_ms(self, now_ms):
        return max(0, now_ms - self.start_ms)

    def state(self, now_ms):
        return self.position(self.elapsed_ms(now_ms) / 1000.0)

    def position(self, t_s):
        """ff::simPeerAt(), as (lat_deg, lon_deg, alt_m, speed_cms, course_ddeg)
        after t seconds on the path."""
        lat, lon = self.lat, self.lon
        alt_m = self.alt_m
        speed_cms = 0
        course_ddeg = course_to_ddeg(self.course_deg)
        travelled = self.speed_ms * t_s

        if self.mode == "static":
            pass
        elif self.mode == "line":
            lat, lon = point_at_distance(self.lat, self.lon, travelled, self.course_deg)
            speed_cms = int(round(self.speed_ms * 100.0))
        elif self.mode == "circle":
            if self.radius_m > 0.0:
                # Angle subtended so far, as seen from the centre; the heading is
                # the tangent, 90 degrees off the radial.
                angle_deg = math.degrees(travelled / self.radius_m)
                lat, lon = point_at_distance(self.lat, self.lon, self.radius_m, angle_deg)
                course_ddeg = course_to_ddeg(angle_deg + 90.0)
                speed_cms = int(round(self.speed_ms * 100.0))
        else:  # hex
            side = self.radius_m
            if side > 0.0:
                perimeter = side * HEX_SIDES
                along = math.fmod(travelled, perimeter)
                if along < 0.0:
                    along += perimeter
                leg = int(along / side) % HEX_SIDES
                leg_progress = along - leg * side
                # A regular hexagon's vertices sit one side length from the
                # centre, 60 degrees apart; the edge leaving vertex i runs at
                # 120 + 60i.
                vertex_bearing = leg * (360.0 / HEX_SIDES)
                edge_bearing = vertex_bearing + 120.0
                vlat, vlon = point_at_distance(self.lat, self.lon, side, vertex_bearing)
                lat, lon = point_at_distance(vlat, vlon, leg_progress, edge_bearing)
                # Altitude ramps linearly to a peak at the half-way vertex and
                # back, so a follower sees a real climb and descent, not a step.
                half = perimeter / 2.0
                frac = (along / half) if along <= half else ((perimeter - along) / half)
                alt_m = int(round(self.alt_m + SIM_HEX_CLIMB_M * frac))
                course_ddeg = course_to_ddeg(edge_bearing)
                speed_cms = int(round(self.speed_ms * 100.0))

        return lat, lon, alt_m, speed_cms, course_ddeg

    # -- serialisation ------------------------------------------------------

    def sim_json(self, now_ms, running):
        """GET /api/sim's peer shape (WebServer.cpp). lat/lon are the configured
        decimal degrees, the same units the POST body takes."""
        return {
            "uid": uid_str(self.uid),
            "name": self.name,
            "mode": self.mode,
            "lat": self.lat,
            "lon": self.lon,
            "alt_m": self.alt_m,
            "speed_ms": self.speed_ms,
            "course_deg": self.course_deg,
            "radius_m": self.radius_m,
            "elapsed_ms": self.elapsed_ms(now_ms),
            "running": bool(running),
        }


# ---------------------------------------------------------------------------
# The mock node
# ---------------------------------------------------------------------------

NODE_UID = 0x1A2B3C4D
NODE_VERSION = "v2.0.0-mock"

# The channel a router puts us on once we join an external network, which is
# what makes the actual and the configured channel differ in station mode.
ROUTER_CHANNEL = 6
HOME_LAT = 37.0
HOME_LON = -122.0
HOME_ALT_M = 120.0

# One frame every FRAME_INTERVAL_MS of node time, so the 32-entry ring rolls in
# about four seconds: fast enough that the debug view is obviously live, slow
# enough that a human can read it.
FRAME_INTERVAL_MS = 120
# Ceiling on frames generated in one catch-up pass, so a server that sat idle
# for an hour does not spend it replaying history.
FRAME_CATCHUP_BUDGET = 200

# Follow's IDLE -> ACQUIRING -> LOCKED walk, in node-uptime milliseconds.
FOLLOW_ACQUIRE_AT_MS = 1500
FOLLOW_LOCK_AT_MS = 6000

# WebServer::kSaveMinIntervalMs -- minimum gap between config.json writes.
SAVE_MIN_INTERVAL_MS = 2000
# How long after answering the node actually reboots (WebServer.cpp).
REBOOT_DELAY_MS = 500

RADIO_ESPNOW, RADIO_LORA, RADIO_SIM = 0, 1, 2
RADIO_NAMES = {RADIO_ESPNOW: "ESPNOW", RADIO_LORA: "LORA", RADIO_SIM: "SIM"}
RADIO_AIRTIME_MS = {RADIO_ESPNOW: 0.4, RADIO_LORA: 61.2, RADIO_SIM: 0.0}

# ff::RadioDriver::Info (radio_hub.h), as RadioSX127x::info() fills it in: read
# back from the driver, not from the config, which is why `power_dbm` is the
# LORA_POWER build flag rather than radios.lora_power_dbm. A node can therefore
# report a transmit power the config never mentions, and that is the point --
# confirming the radio really is where the target intended is the first thing
# worth checking on a node that is not hearing anyone.
#
# These are the 915 numbers this project ships (platformio.ini, env_common_915):
# SF8 and 4/7 rather than SF7 and 4/5, because 902-928 is full of
# frequency-hopping traffic and there is no duty-cycle ceiling to pay for it.
LORA_MODULATION = {
    "frequency_hz": 920000000,   # LORA_FREQUENCY
    "bandwidth_khz": 500.0,      # LORA_BW_KHZ
    "spreading_factor": 8,       # LORA_SF
    "coding_rate": 7,            # LORA_CR, a denominator: 7 means 4/7
    "power_dbm": 10,             # LORA_POWER
}
# Only the LoRa drivers report SNR (Info::has_snr), and only once they have
# actually received a frame. ESP-NOW and the simulated radio never do.
LORA_SNR_DB = 9.5

# The `system` block (fillSystem()). This mock's node is an ESP32 -- it reports
# min_free_heap, which only the ESP32 branch sends -- but it also sends
# heap_fragmentation_pct, which only the ESP8266 branch sends, so the UI's
# fragmentation row is reachable without an 8285 on the bench. Real firmware
# sends one or the other, never both.
SYSTEM_CPU_MHZ = 240
SYSTEM_FLASH_SIZE = 4 * 1024 * 1024
SYSTEM_SKETCH_SIZE = 962192
SYSTEM_FREE_SKETCH_SPACE = 1310720
# esp_reset_reason() through resetReasonName() (WebServer.cpp). A mock that has
# not rebooted yet was powered on; one that has was restarted in software,
# which is exactly what /api/system/reboot and a finished OTA do.
RESET_REASON_POWERON = "power on"
RESET_REASON_SOFTWARE = "software restart"

# Main-loop timing (ff::LoopStats). A few hundred microseconds an iteration,
# with the occasional long one: the mean hides exactly the spikes that matter,
# so the mock has to produce spikes for max and overruns to mean anything.
LOOP_MEAN_US = 320
LOOP_MIN_US = 96
LOOP_SPIKE_EVERY_MS = 4500      # a few-millisecond loop, well under the threshold
LOOP_OVERRUN_EVERY_MS = 23000   # a loop long enough to miss a transmission
LOOP_EVENT_BUDGET = 64          # cap on spikes replayed in one catch-up pass

# One log line every LOG_INTERVAL_MS of node time: slow enough to read, fast
# enough that the 48-entry ring has rolled by the time anyone looks twice.
LOG_INTERVAL_MS = 1700
LOG_CATCHUP_BUDGET = 100


def _new_radio_stats():
    return {"tx": 0, "rx_ok": 0, "rx_crypto_fail": 0, "rx_replay": 0,
            "rx_decode_fail": 0, "rx_self": 0, "last_rx_ms": 0, "last_rssi": 0,
            "rx_dropped": 0, "tx_dropped": 0}


# Frames lost inside the node rather than on the air, faked on LoRa only: 61 ms
# of airtime per frame is what actually runs a node out of budget first, and
# keeping one radio clean is what makes the UI's non-zero styling legible.
LORA_TX_DROP_EVERY = 3    # one in three LoRa transmits finds the radio still busy
LORA_RX_DROP_EVERY = 23   # receive ring overruns, in frames


class MockNode:
    """Everything the API reads from. One lock, one `advance()` that brings the
    world up to the current wall clock, called at the top of every handler."""

    def __init__(self, config=None, seed=1337, power_present=True, power_usb=True):
        self.lock = threading.RLock()
        self.seed = seed
        self.rng = random.Random(seed)
        self.config = config if config is not None else default_config()
        self.saved_config = copy.deepcopy(self.config)
        self.reboots = 0
        # The mock's config store always parses; kept so the status document has
        # the field the UI's "stored config is corrupt" banner reads.
        self.config_corrupt = False

        # Board power. The default is the bench case: a T-Beam whose AXP192
        # answered, on USB, with a battery attached and charging. Flip
        # power_present to False to get the board the firmware omits the whole
        # `power` object for -- which the UI has to read as "no PMIC", not as
        # "zero volts".
        self.power_present = bool(power_present)
        self.power_usb = bool(power_usb)
        self.power_battery_present = True
        # False mirrors a PMIC that will not estimate a charge percentage, in
        # which case `battery_pct` is omitted rather than sent as 0.
        self.power_estimates_pct = True

        # GNSS fix quality. hdop is x100, as ff::NodeLocation carries it, and 0
        # means "this source does not report it" -- the firmware then omits the
        # field entirely rather than publishing a perfect 0.00.
        self.sats = 12
        self.hdop_x100 = 131

        self._boot()

    # -- lifecycle ----------------------------------------------------------

    def _boot(self):
        self._t0 = time.monotonic()
        self.frames = deque(maxlen=FRAME_LOG_CAPACITY)
        self.frame_total = 0
        self.frame_seq = 0
        self.next_frame_ms = 0
        self.stats = {"beacons_sent": 0, "announces_sent": 0, "rx_ok": 0,
                      "rx_rejected": 0, "rx_self": 0}
        self.radio_stats = {RADIO_ESPNOW: _new_radio_stats(),
                            RADIO_LORA: _new_radio_stats(),
                            RADIO_SIM: _new_radio_stats()}
        self.tx_counter = 0
        self.rf_peers = self._default_rf_peers()
        self.sim_peers = {}
        self.follow_state = 0
        self.have_last_target = False
        self.locked_uid = 0
        self.locked_name = ""
        self.reboot_required = False
        self.ever_saved = False
        self.last_save_ms = 0

        # -- in-RAM log (ff::LogRing) ---------------------------------------
        # A power cycle clears all of it, counters included; a *clear* does not.
        self.log = deque(maxlen=LOG_CAPACITY)
        self.log_total = 0
        self.log_warnings = 0
        self.log_errors = 0
        self.log_seq = 0
        self.next_log_ms = 0
        # The firmware's default is Info, so a stock build never records a debug
        # line; a build that wants them calls setMinLevel(Debug). The mock runs
        # at Debug so the log view's per-level styling is reachable here.
        self.log_min_level = "debug"

        # -- main loop (ff::LoopStats) --------------------------------------
        # Its own RNG: the RSSI stream is seeded and several tests read it, and
        # loop jitter has no business perturbing it.
        self.loop_rng = random.Random(self.seed ^ 0xA5)
        # -1 rather than 0 so the very first advance() records a sample even if
        # it lands on uptime 0; a status document with no loop samples in it at
        # all would be a state real firmware never shows.
        self.loop_last_ms = -1
        self.loop_samples = 0
        self.loop_total_us = 0
        self.loop_last_us = 0
        self.loop_min_us = 0
        self.loop_max_us = 0
        self.loop_overruns = 0
        # main.cpp: the threshold is the shortest beacon interval, snapshotted
        # at boot. A loop longer than that can miss a transmission outright.
        self.loop_overrun_us = int(self.config["rate"].get("min_interval_ms", 100)) * 1000
        self.loop_next_spike_ms = LOOP_SPIKE_EVERY_MS
        self.loop_next_overrun_ms = LOOP_OVERRUN_EVERY_MS

        # -- system ----------------------------------------------------------
        self.min_free_heap = self.free_heap(0)

        # main.cpp's first line, and BoardPower's. A node that has just come up
        # should have something in the log before anything else happens.
        self.log_add("info", f"FormationFlight {NODE_VERSION} booting, uid {uid_str(NODE_UID)}", 0)
        if self.power_present:
            self.log_add("info", "AXP192 up: GPS, LoRa and peripheral rails on", 0)
        else:
            self.log_add("error", "AXP192 not found: GPS and LoRa rails are unpowered", 0)

    # -- the in-RAM log -----------------------------------------------------

    def log_add(self, level, text, ms=None):
        """ff::LogRing::vadd(): below the minimum level nothing happens at all,
        not even to the counters; above it the text truncates rather than
        overflowing, and `total` counts everything ever recorded."""
        if LOG_LEVELS.index(level) < LOG_LEVELS.index(self.log_min_level):
            return
        if ms is None:
            ms = self.uptime_ms()
        self.log.append({"ms": int(ms), "level": level, "text": text[:LOG_TEXT_LEN]})
        self.log_total += 1
        if level == "warn":
            self.log_warnings += 1
        elif level == "error":
            self.log_errors += 1

    def clear_log(self):
        """LogRing::clear(): the ring empties, the counters do not. `total`,
        `warnings` and `errors` are since-boot counts, and a client's cursor
        must never walk backwards -- a clear that reset `total` would make every
        outstanding `since` ask for entries that no longer exist."""
        self.log.clear()

    def log_json(self, since=None):
        """GET /api/log. Newest first, like the frame log, and `since` is
        compared against the sequence number entry `i` implies."""
        entries = []
        for i, entry in enumerate(reversed(self.log)):
            seq = self.log_total - 1 - i
            # A `since` of 0, like an absent one, is the whole ring.
            if since and seq < since:
                break
            entries.append(dict(entry))
        return {
            "total": self.log_total,
            "capacity": LOG_CAPACITY,
            "warnings": self.log_warnings,
            "errors": self.log_errors,
            "entries": entries,
        }

    def _emit_log(self, ms):
        """One line of the trickle. Mostly routine, with a warning often enough
        that the log view's level styling shows up within a minute and an error
        rarely enough that it still reads as an event rather than as wallpaper.
        The content mirrors things this mock is actually doing."""
        seq = self.log_seq
        self.log_seq += 1
        peers = self._peer_list()
        peer = peers[seq % len(peers)] if peers else None
        name = peer.name if peer is not None else "?"
        uid = uid_str(peer.uid) if peer is not None else uid_str(0)

        if seq % 29 == 28:
            self.log_add("error", "AXP192 read failed, rails may be unpowered", ms)
        elif seq % 11 == 7:
            self.log_add("warn", f"crypto: bad tag from {uid}, frame dropped", ms)
        elif seq % 11 == 3:
            self.log_add("warn", "LoRa tx dropped: radio still busy", ms)
        elif seq % 5 == 1:
            self.log_add("info", f"peer {uid} {name} seen on "
                                 f"{RADIO_NAMES[peer.radios[0]] if peer else 'ESPNOW'}", ms)
        elif seq % 5 == 2:
            self.log_add("debug", f"rate: {len(peers)} peers, beacon every "
                                  f"{self._beacon_interval_ms(len(peers), RADIO_LORA)} ms", ms)
        elif seq % 5 == 3:
            self.log_add("debug", f"msp: radar out for {len(peers)} peers", ms)
        else:
            self.log_add("info", "wifi: 1 client on the AP, channel "
                                 f"{self.wifi_json()['channel']}", ms)

    def save_allowed(self, now_ms):
        """WebServer::saveAllowed(): flash has a finite number of erase cycles
        and a UI with a Save button has an infinite number of clicks."""
        return not self.ever_saved or (now_ms - self.last_save_ms) >= SAVE_MIN_INTERVAL_MS

    def mark_saved(self, now_ms):
        self.ever_saved = True
        self.last_save_ms = now_ms

    def reboot(self):
        """Power cycle: the saved config comes back, RAM-only edits do not."""
        with self.lock:
            self.reboots += 1
            self.config = copy.deepcopy(self.saved_config)
            self._boot()

    def _default_rf_peers(self):
        return {
            0xAABBCCDD: MovingPeer(0xAABBCCDD, "XYZ", "circle", HOME_LAT, HOME_LON,
                                   HOME_ALT_M + 10, 18.0, 30.0, 180.0,
                                   radios=(RADIO_ESPNOW, RADIO_LORA), armed=True),
            0x5A1B2C3D: MovingPeer(0x5A1B2C3D, "HAWK", "hex", HOME_LAT, HOME_LON,
                                   HOME_ALT_M - 20, 12.0, 45.0, 220.0,
                                   radios=(RADIO_ESPNOW,), armed=False),
        }

    # -- clock --------------------------------------------------------------

    def uptime_ms(self):
        return int((time.monotonic() - self._t0) * 1000.0)

    def free_heap(self, now_ms):
        """ESP.getFreeHeap(). Wobbles like a real heap under fragmentation, so a
        UI graphing it has something with texture. `node.free_heap` and
        `system.free_heap` are the same call on the firmware, so they are the
        same number here."""
        return 24160 + (now_ms // 997) % 512 - 256

    # -- world tick ---------------------------------------------------------

    def advance(self):
        """Generate whatever frames are due. Cheap and idempotent; call it from
        every handler before reading state."""
        with self.lock:
            now = self.uptime_ms()
            budget = FRAME_CATCHUP_BUDGET
            while self.next_frame_ms <= now and budget > 0:
                self._emit_frame(self.next_frame_ms)
                self.next_frame_ms += FRAME_INTERVAL_MS
                budget -= 1
            if self.next_frame_ms < now:
                # Server was idle for a long time; skip the backlog rather than
                # replay it.
                self.next_frame_ms = now

            budget = LOG_CATCHUP_BUDGET
            while self.next_log_ms <= now and budget > 0:
                self._emit_log(self.next_log_ms)
                self.next_log_ms += LOG_INTERVAL_MS
                budget -= 1
            if self.next_log_ms < now:
                self.next_log_ms = now

            self._advance_loop(now)
            self.min_free_heap = min(self.min_free_heap, self.free_heap(now))
            self._update_follow(now)
            return now

    def _advance_loop(self, now_ms):
        """Feed ff::LoopStats from a loop that really is running.

        The spikes are scheduled against node time rather than against how often
        a handler called this, so the numbers do not change shape when the UI
        polls harder -- and a node left alone for a minute comes back with the
        overruns it would have collected while nobody was looking.
        """
        if now_ms <= self.loop_last_ms:
            return
        elapsed_us = (now_ms - self.loop_last_ms) * 1000
        self.loop_last_ms = now_ms

        mean_us = self.loop_rng.uniform(LOOP_MEAN_US * 0.85, LOOP_MEAN_US * 1.15)
        iterations = max(1, int(elapsed_us / mean_us))
        self.loop_samples += iterations
        self.loop_total_us += int(iterations * mean_us)

        self.loop_last_us = int(self.loop_rng.uniform(LOOP_MIN_US * 1.4, LOOP_MEAN_US * 1.6))
        low = int(self.loop_rng.uniform(LOOP_MIN_US, LOOP_MIN_US * 1.3))
        self.loop_min_us = low if self.loop_min_us == 0 else min(self.loop_min_us, low)
        # The worst iteration in a batch is always somewhat above that batch's
        # mean, so max_us can never end up under mean_us.
        self.loop_max_us = max(self.loop_max_us, self.loop_last_us, int(mean_us * 1.25))

        budget = LOOP_EVENT_BUDGET
        while self.loop_next_spike_ms <= now_ms and budget > 0:
            # Long, but not long enough to miss a beacon: this is the number the
            # mean hides, which is the whole reason max_us is published.
            self.loop_max_us = max(self.loop_max_us,
                                   int(self.loop_rng.uniform(2000, 16000)))
            self.loop_next_spike_ms += LOOP_SPIKE_EVERY_MS
            budget -= 1
        if self.loop_next_spike_ms < now_ms:
            self.loop_next_spike_ms = now_ms + LOOP_SPIKE_EVERY_MS

        while self.loop_next_overrun_ms <= now_ms and budget > 0:
            over = int(self.loop_overrun_us * self.loop_rng.uniform(1.05, 2.4))
            self.loop_max_us = max(self.loop_max_us, over)
            self.loop_overruns += 1
            self.loop_next_overrun_ms += LOOP_OVERRUN_EVERY_MS
            budget -= 1
        if self.loop_next_overrun_ms < now_ms:
            self.loop_next_overrun_ms = now_ms + LOOP_OVERRUN_EVERY_MS

    def active_peers(self):
        """Peers that show up in the peer table: the built-in RF ones always,
        plus the simulated ones while sim.enabled."""
        peers = dict(self.rf_peers)
        if self.config["sim"]["enabled"]:
            peers.update(self.sim_peers)
        return peers

    def _peer_list(self):
        return list(self.active_peers().values())

    def _emit_frame(self, ms):
        seq = self.frame_seq
        self.frame_seq += 1
        peers = self._peer_list()

        # Every fifth frame is one of ours going out.
        if seq % 5 == 0 or not peers:
            radio = RADIO_ESPNOW if (seq // 5) % 4 else RADIO_LORA
            if not self._radio_enabled(radio):
                radio = self._first_enabled_radio()
            announce = (seq // 5) % 16 == 0
            # Refused by the driver because the previous frame is still going
            # out. Nothing is transmitted and nothing is logged: the frame log
            # is the main loop's view, and this frame never left the driver.
            # Offset so the node's very first transmit is never the dropped one
            # -- a freshly started mock should look alive immediately.
            if radio == RADIO_LORA and (seq // 20) % LORA_TX_DROP_EVERY == 2:
                self.radio_stats[radio]["tx_dropped"] += 1
                return
            self.radio_stats[radio]["tx"] += 1
            self.tx_counter += 1
            if announce:
                self.stats["announces_sent"] += 1
            else:
                self.stats["beacons_sent"] += 1
            self._log_frame(ms, radio, NODE_UID, 2 if announce else 1,
                            ANNOUNCE_FRAME_SIZE if announce else POSITION_FRAME_SIZE,
                            0, "tx")
            return

        peer = peers[seq % len(peers)]
        radio = peer.radios[(seq // len(peers)) % len(peer.radios)]
        if not self._radio_enabled(radio):
            radio = self._first_enabled_radio()
        rs = self.radio_stats[radio]

        # Thrown away by the driver because its receive ring filled before the
        # main loop drained it. Invisible everywhere else, including the frame
        # log, which only ever sees what the main loop was handed.
        if radio == RADIO_LORA and seq % LORA_RX_DROP_EVERY == 0:
            rs["rx_dropped"] += 1
            return

        # A believable error mix: mostly good frames, a crypto_fail often enough
        # that the debug view's error styling is visible within a few seconds.
        if seq % 47 == 0:
            result, uid, ptype = "crypto_fail", 0, 0
            rs["rx_crypto_fail"] += 1
            self.stats["rx_rejected"] += 1
        elif seq % 83 == 0:
            result, uid, ptype = "replay_fail", peer.uid, 1
            rs["rx_replay"] += 1
            self.stats["rx_rejected"] += 1
        elif seq % 101 == 0:
            result, uid, ptype = "decode_fail", peer.uid, 0
            rs["rx_decode_fail"] += 1
            self.stats["rx_rejected"] += 1
        elif seq % 31 == 0:
            result, uid, ptype = "self", NODE_UID, 1
            rs["rx_self"] += 1
            self.stats["rx_self"] += 1
        else:
            result, uid, ptype = "ok", peer.uid, 1 if seq % 13 else 2
            rs["rx_ok"] += 1
            self.stats["rx_ok"] += 1
            peer.packets += 1
            peer.last_rx_ms = ms

        peer.rssi = self._peer_rssi(peer, ms)
        rs["last_rx_ms"] = ms
        rs["last_rssi"] = peer.rssi
        self._log_frame(ms, radio, uid, ptype,
                        ANNOUNCE_FRAME_SIZE if ptype == 2 else POSITION_FRAME_SIZE,
                        peer.rssi, result)

    def _peer_rssi(self, peer, ms):
        lat, lon, _alt, _spd, _crs = peer.state(ms)
        d = max(10.0, distance_meters(HOME_LAT, HOME_LON, lat, lon))
        rssi = -42.0 - 20.0 * math.log10(d / 10.0) + self.rng.uniform(-2.5, 2.5)
        return int(max(-120.0, min(-20.0, rssi)))

    def _log_frame(self, ms, radio, uid, ptype, length, rssi, result):
        self.frame_total += 1
        self.frames.append({
            "seq": self.frame_total,   # internal; stripped on the way out
            "ms": ms,
            "radio": radio,
            "uid": uid_str(uid),
            "type": ptype,
            "len": length,
            "rssi": int(rssi),
            "result": result,
        })

    def _radio_enabled(self, index):
        if index == RADIO_ESPNOW:
            return bool(self.config["radios"]["espnow_enabled"])
        if index == RADIO_LORA:
            return bool(self.config["radios"]["lora_enabled"])
        return bool(self.config["sim"]["enabled"])

    def _first_enabled_radio(self):
        for index in (RADIO_ESPNOW, RADIO_LORA, RADIO_SIM):
            if self._radio_enabled(index):
                return index
        return RADIO_ESPNOW

    def _radio_indices(self):
        indices = [RADIO_ESPNOW, RADIO_LORA]
        if self.config["sim"]["enabled"]:
            indices.append(RADIO_SIM)
        return indices

    # -- Follow -------------------------------------------------------------

    def _followable(self, now_ms):
        """A peer is followable when its last position is fresher than
        follow.peerTimeoutMs -- ff::followPeerStale()."""
        timeout = self.config["follow"].get("peerTimeoutMs", 1500)
        out = []
        for peer in self._peer_list():
            if peer.last_rx_ms and (now_ms - peer.last_rx_ms) <= timeout:
                out.append(peer)
        return out

    def _resolve_lock(self, now_ms):
        candidates = self._followable(now_ms)
        if not candidates:
            return None
        target_uid = self.config["follow"].get("targetUid", 0)
        if target_uid:
            for peer in candidates:
                if peer.uid == target_uid:
                    return peer
            return None
        # 0 = nearest followable peer.
        def dist(p):
            lat, lon, _a, _s, _c = p.state(now_ms)
            return distance_meters(HOME_LAT, HOME_LON, lat, lon)
        return min(candidates, key=dist)

    def _update_follow(self, now_ms):
        if now_ms < FOLLOW_ACQUIRE_AT_MS:
            self.follow_state = 0          # IDLE, gate not up yet
            self.locked_uid = 0
            self.locked_name = ""
            return
        peer = self._resolve_lock(now_ms)
        if peer is None or now_ms < FOLLOW_LOCK_AT_MS:
            self.follow_state = 1          # ACQUIRING
            self.locked_uid = 0
            self.locked_name = ""
            return
        self.follow_state = 2              # LOCKED
        self.locked_uid = peer.uid
        self.locked_name = peer.name
        self.have_last_target = True

    def follow_json(self, now_ms):
        """The `follow` block of /api/status, with the same key set and the same
        presence rules as WebServer.cpp's fillStatus()."""
        cfg = self.config["follow"]
        platform = 1  # FcPlatform::Airplane
        # autothrottleArmed()'s "no arm channel wired" fallback: this mock has no
        # real RC channel data to read, so an unassigned arm channel reads armed.
        armed = cfg.get("autothrottleEnableRcChannel", -1) == -1

        doc = {
            "state": FOLLOW_STATE_NAMES[self.follow_state],
            "gate_active": now_ms >= FOLLOW_ACQUIRE_AT_MS,
            # Always present, zero/empty when nothing is locked, exactly as the
            # firmware serialises FollowStatus.
            "locked_uid": uid_str(self.locked_uid),
            "locked_name": self.locked_name,
            "platform": platform,
            "autothrottle_armed": armed,
            "rc_slot_frozen": False,
            "prearm_failed": False,
        }

        locked = self.follow_state in (2, 3)
        peer = self.active_peers().get(self.locked_uid) if locked else None
        # The target is recomputed at emitHz; age_ms is time since that cycle.
        emit_hz = max(1, cfg.get("emitHz", 4))
        period_ms = max(1, int(1000 / emit_hz))
        age_ms = now_ms % period_ms

        if peer is not None and self.have_last_target:
            lat, lon, alt, speed_cms, course_ddeg = peer.state(now_ms - age_ms)
            course = course_ddeg / 10.0
            # slotToLatLon(): project the track-relative slot onto the ground.
            long_m, lat_m = cfg.get("ofsLongM", 0.0), cfg.get("ofsLatM", 0.0)
            t_lat, t_lon = point_at_distance(lat, lon, abs(long_m),
                                             course if long_m >= 0 else course + 180.0)
            if lat_m:
                t_lat, t_lon = point_at_distance(t_lat, t_lon, abs(lat_m),
                                                 course + (90.0 if lat_m >= 0 else -90.0))
            alt_m = max(cfg.get("minAltM", 0.0), alt + cfg.get("ofsVertM", 0.0))
            doc["target"] = {
                "lat": to_1e7(t_lat),
                "lon": to_1e7(t_lon),
                "alt_cm": int(round(alt_m * 100.0)),
                "heading_deg": self._follow_heading(cfg, course, t_lat, t_lon),
                "age_ms": age_ms,
            }
            doc["live_offset"] = {
                "long_m": cfg.get("ofsLongM", 0.0),
                "lat_m": cfg.get("ofsLatM", 0.0),
                "vert_m": cfg.get("ofsVertM", 0.0),
            }
            # Only meaningful alongside a target, so the firmware only emits
            # these two when it has one.
            doc["autothrottle_engaged"] = bool(armed and platform == 1)
            doc["target_speed_cms"] = int(speed_cms) if doc["autothrottle_engaged"] else 0

        # Absent while that GVAR slot is disabled, rather than present and zero.
        if cfg.get("statusGvarIndex", -1) >= 0:
            doc["status_gvar"] = self.follow_state
        if cfg.get("conditionFlagsGvarIndex", -1) >= 0:
            doc["condition_gvar"] = 0
        if any(cfg.get(k, -1) != -1 for k in ("rcLongChannel", "rcLatChannel", "rcVertChannel")):
            doc["prearm_offset"] = {
                "long_m": cfg.get("ofsLongM", 0.0),
                "lat_m": cfg.get("ofsLatM", 0.0),
                "vert_m": cfg.get("ofsVertM", 0.0),
            }
        return doc

    def _follow_heading(self, cfg, course_deg, target_lat, target_lon):
        """WP#255's p1: 1-360, or 0 for 'leave heading alone'."""
        mode = cfg.get("headingMode", "POINT_LEADER")
        if mode == "OFF":
            return 0
        if mode == "COURSE":
            h = course_deg
        elif mode == "FIXED":
            h = cfg.get("headingDeg", 0.0)
        elif mode == "COURSE_RELATIVE":
            h = course_deg + cfg.get("headingDeg", 0.0)
        else:  # POINT_LEADER
            h = bearing_degrees(HOME_LAT, HOME_LON, target_lat, target_lon)
        h = int(round(h)) % 360
        return 360 if h == 0 else h

    # -- status -------------------------------------------------------------

    def node_name(self):
        name = self.config["node"].get("name") or ""
        if name:
            return name
        # config.h: an empty name means "derive from the UID at boot".
        return f"{NODE_UID & 0xFFF:03X}"

    def self_location(self, now_ms):
        t = now_ms / 1000.0
        lat, lon = point_at_distance(HOME_LAT, HOME_LON, 15.0 * t, 90.0)
        return lat, lon, HOME_ALT_M

    def wifi_json(self):
        """The `wifi` block of fillStatus(). ESP-NOW rides whatever channel the
        WiFi radio ended up on, so `channel` is the one that matters and
        `configured_channel` is only what was asked for.

        Joining an external network hands the choice to the router, which is
        exactly how the two come apart on real hardware -- so the mock's
        station mode lands on a different channel on purpose, to give the UI's
        mismatch warning something to show.
        """
        cfg = self.config["wifi"]
        ap_only = bool(cfg.get("ap"))
        configured = int(cfg.get("channel", 1))
        wifi = {
            "mode": "ap" if ap_only else "ap_sta",
            "channel": configured if ap_only else ROUTER_CHANNEL,
            "configured_channel": configured,
            # Whoever has this page open.
            "ap_clients": 1,
        }
        if not ap_only:
            wifi["sta_connected"] = True
            wifi["sta_rssi"] = -57
        return wifi

    def system_json(self, now_ms):
        """fillSystem(). Why the node last restarted is the single most useful
        thing to know about a board that has been misbehaving, and it is
        invisible everywhere else."""
        free = self.free_heap(now_ms)
        # A heap that is never quite contiguous: a web request can fail for want
        # of one large block while plenty of heap is nominally "free", which is
        # the failure heap_fragmentation_pct exists to explain.
        largest = int(free * (0.80 + 0.05 * math.sin(now_ms / 11000.0)))
        return {
            "cpu_mhz": SYSTEM_CPU_MHZ,
            "free_heap": free,
            "sketch_size": SYSTEM_SKETCH_SIZE,
            "free_sketch_space": SYSTEM_FREE_SKETCH_SPACE,
            "reset_reason": RESET_REASON_POWERON if self.reboots == 0
                            else RESET_REASON_SOFTWARE,
            "largest_free_block": largest,
            "flash_size": SYSTEM_FLASH_SIZE,
            # ESP8266-only on real firmware; see LORA_MODULATION's neighbours
            # above for why this mock sends both halves.
            "heap_fragmentation_pct": max(0, int(round(100.0 * (1.0 - largest / free)))),
            # ESP32-only on real firmware.
            "min_free_heap": self.min_free_heap,
        }

    def loop_json(self):
        """ff::LoopStats, as fillStatus() serialises it. minUs() and rateHz()
        read 0 before the first sample, so a node that has not looped yet says
        so rather than claiming a 0 us loop."""
        mean = int(self.loop_total_us // self.loop_samples) if self.loop_samples else 0
        return {
            "last_us": self.loop_last_us,
            "min_us": self.loop_min_us if self.loop_samples else 0,
            "max_us": self.loop_max_us,
            "mean_us": mean,
            "rate_hz": (1000000 // mean) if mean else 0,
            "samples": self.loop_samples,
            "overruns": self.loop_overruns,
            "overrun_threshold_us": self.loop_overrun_us,
        }

    def power_json(self, now_ms):
        """The `power` block, or None on a board the firmware omits it for.

        Charge and discharge are separate readings on the PMIC, so a node on USB
        with a battery attached shows a supply voltage *and* a charge current --
        the state people most often misread. The mock keeps the two cases
        internally consistent rather than emitting a plausible-looking
        impossibility like charging at 0 V of supply.
        """
        if not self.power_present:
            return None
        wobble = math.sin(now_ms / 30000.0)
        battery = self.power_battery_present
        if self.power_usb:
            battery_v = round(4.06 + 0.02 * wobble, 3) if battery else 0.0
            doc = {
                "battery_v": battery_v,
                "supply_v": round(5.05 + 0.03 * wobble, 3),
                # Tapering, the way a PMIC's constant-voltage phase does.
                "charge_ma": round(240.0 + 30.0 * wobble, 1) if battery else 0.0,
                "discharge_ma": 0.0,
                "pmic_temp_c": round(31.5 + 1.5 * wobble, 1),
                "battery_present": battery,
                "charging": battery,
                "usb_present": True,
            }
        else:
            battery_v = round(3.92 + 0.03 * wobble, 3) if battery else 0.0
            doc = {
                "battery_v": battery_v,
                "supply_v": 0.0,
                "charge_ma": 0.0,
                "discharge_ma": round(310.0 + 40.0 * wobble, 1) if battery else 0.0,
                "pmic_temp_c": round(28.0 + 1.5 * wobble, 1),
                "battery_present": battery,
                "charging": False,
                "usb_present": False,
            }
        # Omitted, not zeroed, when the PMIC will not estimate one: 0% and "no
        # estimate" are very different things to show a pilot.
        if battery and self.power_estimates_pct:
            doc["battery_pct"] = self._battery_pct(battery_v)
        return doc

    @staticmethod
    def _battery_pct(battery_v):
        """A single-cell lithium curve flattened to a line. Crude on purpose:
        the AXP192's own estimate is not much better."""
        pct = (battery_v - 3.30) / (4.15 - 3.30) * 100.0
        return int(max(0, min(100, round(pct))))

    def status_json(self, now_ms):
        lat, lon, alt = self.self_location(now_ms)
        peers = self._peer_list()
        cfg = self.config

        radios = []
        for index in self._radio_indices():
            rs = self.radio_stats[index]
            on_radio = [p for p in peers if index in p.radios]
            radio = {
                "index": index,
                "name": RADIO_NAMES[index],
                "enabled": self._radio_enabled(index),
                "sim": index == RADIO_SIM,
                "tx": rs["tx"],
                "rx_ok": rs["rx_ok"],
                "rx_crypto_fail": rs["rx_crypto_fail"],
                "rx_replay": rs["rx_replay"],
                "rx_decode_fail": rs["rx_decode_fail"],
                "rx_self": rs["rx_self"],
                "last_rssi": rs["last_rssi"],
                "last_rx_age_ms": max(0, now_ms - rs["last_rx_ms"]) if rs["last_rx_ms"] else 0,
                "beacon_interval_ms": self._beacon_interval_ms(len(on_radio), index),
                "airtime_ms": RADIO_AIRTIME_MS[index],
                "peers": len(on_radio),
                # Frames lost inside the node rather than on the air, so no
                # on-air counter anywhere shows them.
                "rx_dropped": rs["rx_dropped"],
                "tx_dropped": rs["tx_dropped"],
                # False for a receive-only driver; SimRadio is the one here.
                "transmits": index != RADIO_SIM,
            }
            # Only a driver that reports a frequency has a modulation to
            # describe. ESP-NOW's Info is all zeros and the simulated radio
            # never touched an antenna, so neither sends the object at all.
            if index == RADIO_LORA:
                radio["modulation"] = dict(LORA_MODULATION)
                # has_snr goes true on the first frame the driver receives, so
                # a LoRa radio that has not heard anything yet omits it.
                if rs["last_rx_ms"]:
                    radio["last_snr_db"] = round(
                        LORA_SNR_DB + 2.5 * math.sin(now_ms / 9000.0)
                        + (rs["last_rssi"] + 70) / 40.0, 2)
            radios.append(radio)

        peer_docs = []
        for peer in peers:
            p_lat, p_lon, p_alt, speed_cms, course_ddeg = peer.state(now_ms)
            flags = POSITION_FLAG_HAS_FIX | (POSITION_FLAG_ARMED if peer.armed else 0)
            peer_docs.append({
                "uid": uid_str(peer.uid),
                "name": peer.name,
                "lat": to_1e7(p_lat),
                "lon": to_1e7(p_lon),
                "alt_m": p_alt,
                "speed_cms": speed_cms,
                "course_ddeg": course_ddeg,
                "flags": flags,
                "rssi": peer.rssi,
                "age_ms": max(0, now_ms - peer.last_rx_ms) if peer.last_rx_ms else 0,
                "packets": peer.packets,
                "radios": list(peer.radios),
                "distance_m": round(distance_meters(lat, lon, p_lat, p_lon), 1),
                "bearing_deg": int(round(bearing_degrees(lat, lon, p_lat, p_lon))) % 360,
                "rel_alt_m": int(round(p_alt - alt)),
            })

        # The literal passphrase "none" disables the cipher, and with no cipher
        # there are no cipher counters to report -- WebServer.cpp emits `mode`
        # alone in that case.
        passphrase = cfg["security"].get("passphrase", "")
        if passphrase == CRYPTO_DISABLED_PASSPHRASE:
            crypto = {"mode": "none"}
        else:
            crypto = {
                "mode": "ccm",
                "bad_tag": sum(r["rx_crypto_fail"] for r in self.radio_stats.values()),
                "replay": sum(r["rx_replay"] for r in self.radio_stats.values()),
                "tx_counter": self.tx_counter,
            }

        location = {
            "valid": True,
            "source": "msp",
            "lat": to_1e7(lat),
            "lon": to_1e7(lon),
            "alt_m": int(alt),
            "speed_cms": 1500,
            "course_ddeg": 900,
            "armed": False,
            # "no fix" and "a four-satellite fix wandering by 30 m" are very
            # different problems and look identical without these.
            "sats": self.sats,
            # UBX NAV-PVT numbering, the one scheme the API publishes on every
            # source: 0 none, 1 dead reckoning, 2 2D, 3 3D, 4 3D + dead
            # reckoning, 5 time only. The MSP path converts its own 0/1/2 on
            # the way in (mspFixToCanonical, MspFcLink.cpp), so a healthy fix
            # is 3 here even though `source` is "msp".
            "fix_type": 3,
        }
        # Omitted entirely when the source does not report it, rather than sent
        # as a flawless 0.00.
        if self.hdop_x100:
            location["hdop"] = self.hdop_x100 / 100.0

        doc = {
            "node": {
                "uid": uid_str(NODE_UID),
                "name": self.node_name(),
                "version": NODE_VERSION,
                "uptime_ms": now_ms,
                "free_heap": self.free_heap(now_ms),
                "listen_only": bool(cfg["node"].get("listen_only")),
            },
            "location": location,
            "radios": radios,
            "peers": peer_docs,
            "crypto": crypto,
            "stats": dict(self.stats),
            "fc": {
                "connected": True,
                "variant": "INAV",
                "version": "9.1.0",
                "platform": 1,
                "armed": False,
                "gcs_nav": now_ms >= FOLLOW_ACQUIRE_AT_MS,
                "heading_hold": False,
            },
            "follow": self.follow_json(now_ms),
            "sim": {
                "enabled": bool(cfg["sim"]["enabled"]),
                "peers": len(self.sim_peers),
            },
        }
        # Keys from here down are added in fillStatus()'s own order.
        #
        # A board with no PMIC, or one whose PMIC did not answer, sends no
        # `power` object at all -- and on a T-Beam the absence is itself the
        # diagnostic, because the same chip powers the GPS and LoRa rails.
        power = self.power_json(now_ms)
        if power is not None:
            doc["power"] = power
        doc["wifi"] = self.wifi_json()
        doc["system"] = self.system_json(now_ms)
        # Absent on a build with no LoopStats wired to the web server; this mock
        # always has one, the way main.cpp always does.
        doc["loop"] = self.loop_json()
        # The counters only; the entries themselves are GET /api/log.
        doc["log"] = {
            "total": self.log_total,
            "warnings": self.log_warnings,
            "errors": self.log_errors,
        }
        # Not in docs/v2-web-api.md's example, but WebServer.cpp sends both:
        # a setting that only takes effect at boot has been changed, and the
        # stored config could not be parsed at boot (defaults in force, the
        # file left alone for recovery).
        doc["reboot_required"] = self.reboot_required
        doc["config_corrupt"] = self.config_corrupt
        return doc

    def _beacon_interval_ms(self, active_peers, radio_index):
        """RateController::baseIntervalMs(): (N_total * airtime) / G_target,
        clamped to [min, max] (lib/ff_core/rate_control.h)."""
        rate = self.config["rate"]
        airtime = RADIO_AIRTIME_MS[radio_index] or 0.4
        load = rate.get("target_load", 0.15) or 0.15
        base = int(((active_peers + 1) * airtime) / load)
        return max(rate.get("min_interval_ms", 100),
                   min(rate.get("max_interval_ms", 1000), base))

    def frames_json(self, since=None):
        frames = [f for f in reversed(self.frames)]  # newest first
        if since is not None:
            frames = [f for f in frames if f["seq"] > since]
        return {
            "total": self.frame_total,
            "capacity": FRAME_LOG_CAPACITY,
            "frames": [{k: v for k, v in f.items() if k != "seq"} for f in frames],
        }

    # -- simulated peers ----------------------------------------------------

    def sim_json(self, now_ms):
        running = bool(self.config["sim"]["enabled"])
        return {
            "enabled": running,
            "peers": [p.sim_json(now_ms, running) for p in self.sim_peers.values()],
        }

    def upsert_sim_peer(self, body, now_ms):
        """Creates or replaces one simulated peer, mirroring WebServer.cpp's
        /api/sim/peer handler. Returns (peer, None, 200) or (None, error, code).

        Like the firmware this builds a *fresh* SimPeerConfig: a key the body
        leaves out takes the struct's default rather than the existing peer's
        value, so a POST is a replace, not a patch. (The config endpoint merges;
        this one does not, and the difference is deliberate on both sides.)
        """
        if not isinstance(body, dict):
            return None, "body must be a JSON object", 400

        def num(key, fallback):
            """ArduinoJson's `o[key] | default`: the default stands in for a
            missing key and for one that will not convert."""
            v = body.get(key)
            if isinstance(v, bool) or not isinstance(v, (int, float)):
                return fallback
            return float(v)

        # hexToUid(): strtoul, so junk and an absent key both read as 0, and 0
        # means "generate one".
        uid = 0
        if isinstance(body.get("uid"), str):
            uid = parse_uid(body["uid"]) or 0
        if uid == 0:
            # Generated UIDs are marked so a simulated peer is recognisable as
            # one even in a raw packet capture.
            uid = 0x5EED0000 | ((len(self.sim_peers) + 1) & 0xFFFF)

        name = body.get("name")
        name = name if isinstance(name, str) and name else "SIM"

        mode = body.get("mode")
        mode = mode if isinstance(mode, str) else "static"
        if mode not in SIM_MODES:
            return None, "mode must be static, line, circle or hex", 400

        # lat/lon here are decimal degrees, not 1e7 integers, because they are
        # typed by a human into a form (docs/v2-web-api.md).
        speed_ms = num("speed_ms", 0.0)
        if speed_ms < 0.0 or speed_ms > SIM_MAX_SPEED_MS:
            return None, "speed_ms must be 0-200", 400
        radius_m = num("radius_m", 100.0)
        if mode in ("circle", "hex") and radius_m <= 0.0:
            return None, "radius_m must be > 0 for circle and hex", 400

        if uid not in self.sim_peers and len(self.sim_peers) >= SIM_MAX_PEERS:
            return None, "simulated peer table full", 409

        peer = MovingPeer(
            uid=uid,
            name=name,
            mode=mode,
            lat=num("lat", 0.0),
            lon=num("lon", 0.0),
            alt_m=num("alt_m", 100.0),
            speed_ms=speed_ms,
            course_deg=num("course_deg", 0.0),
            radius_m=radius_m,
            radios=(RADIO_SIM,),
            armed=False,          # simPositionPacket() claims a fix, not armed
            sim=True,
            start_ms=now_ms,      # the path starts at the moment of the call
        )
        # Seen right now, so it is immediately followable rather than stale.
        peer.last_rx_ms = now_ms
        peer.rssi = -68
        self.sim_peers[uid] = peer
        return peer, None, 200

    def delete_sim_peer(self, uid):
        return self.sim_peers.pop(uid, None) is not None

    def clear_sim_peers(self):
        n = len(self.sim_peers)
        self.sim_peers.clear()
        return n


# ---------------------------------------------------------------------------
# HTTP
# ---------------------------------------------------------------------------

def make_handler(node, config_path=None, quiet=True):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        server_version = "FormationFlightMock/2"

        # -- plumbing -------------------------------------------------------

        def log_message(self, fmt, *args):
            if not quiet:
                BaseHTTPRequestHandler.log_message(self, fmt, *args)

        def _cors(self):
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS")
            self.send_header("Access-Control-Allow-Headers", "Content-Type")

        def _send(self, body, ctype, status=200):
            if isinstance(body, str):
                body = body.encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self._cors()
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        def _json(self, obj, status=200):
            self._send(json.dumps(obj), "application/json", status)

        # docs/v2-web-api.md: errors are 4xx with a text/plain body naming the
        # offending field. The UI shows that string verbatim.
        def _text(self, text, status=200):
            self._send(text, "text/plain; charset=utf-8", status)

        def _read_body(self):
            length = int(self.headers.get("Content-Length") or 0)
            return self.rfile.read(length) if length else b""

        def _read_json(self):
            raw = self._read_body()
            if not raw:
                return {}, None
            try:
                return json.loads(raw.decode("utf-8")), None
            except (ValueError, UnicodeDecodeError) as exc:
                return None, f"body is not valid JSON: {exc}"

        # -- verbs ----------------------------------------------------------

        def do_OPTIONS(self):
            # CORS preflight, for UI development off-device.
            self.send_response(200)
            self.send_header("Content-Length", "0")
            self._cors()
            self.end_headers()

        def do_GET(self):
            parsed = urlparse(self.path)
            path, query = parsed.path, parse_qs(parsed.query)
            now = node.advance()
            with node.lock:
                if path == "/api/status":
                    self._json(node.status_json(now))
                    return
                if path == "/api/config":
                    self._json(redact_config(node.config))
                    return
                if path == "/api/frames":
                    since = None
                    if "since" in query:
                        try:
                            since = int(query["since"][0])
                        except ValueError:
                            self._text("since must be an integer", 400)
                            return
                    self._json(node.frames_json(since))
                    return
                if path == "/api/log":
                    # The in-RAM log. On an ESP8266 target this is the only safe
                    # log there is: the console UART is the MSP UART, so
                    # printing a diagnostic injects bytes into the flight
                    # controller's serial link.
                    since = None
                    if "since" in query:
                        try:
                            since = int(query["since"][0])
                        except ValueError:
                            self._text("since must be an integer", 400)
                            return
                    self._json(node.log_json(since))
                    return
                if path == "/api/sim":
                    self._json(node.sim_json(now))
                    return
            if path.startswith("/api/"):
                self._text("Not found", 404)
                return
            self._serve_static(path)

        do_HEAD = do_GET

        def do_POST(self):
            parsed = urlparse(self.path)
            path = parsed.path
            now = node.advance()

            if path == "/update":
                self._handle_update()
                return

            if path == "/api/config":
                body, err = self._read_json()
                if err:
                    self._text(err, 400)
                    return
                with node.lock:
                    merged, verr = merge_config(body, node.config)
                    if verr:
                        # Nothing changed.
                        self._text(verr, 400)
                        return
                    node.config = merged
                    self._json(redact_config(node.config))
                return

            if path == "/api/config/save":
                self._read_body()
                with node.lock:
                    if not node.save_allowed(now):
                        self._text("saved too recently, try again shortly", 429)
                        return
                    node.saved_config = copy.deepcopy(node.config)
                    if config_path is not None:
                        try:
                            Path(config_path).write_text(
                                json.dumps(node.saved_config, indent=2), encoding="utf-8")
                        except OSError as exc:
                            self._text(f"config write failed: {exc}", 500)
                            return
                    node.mark_saved(now)
                self._text("saved")
                return

            if path == "/api/config/reset":
                self._read_body()
                with node.lock:
                    node.config = default_config()
                    node.saved_config = copy.deepcopy(node.config)
                    if config_path is not None:
                        try:
                            Path(config_path).write_text(
                                json.dumps(node.saved_config, indent=2), encoding="utf-8")
                        except OSError as exc:
                            self._text(f"config write failed: {exc}", 500)
                            return
                    # Radio and WiFi settings only take effect at boot, and a
                    # factory reset almost certainly changed some.
                    node.reboot_required = True
                self._text("reset")
                return

            if path == "/api/sim/peer":
                body, err = self._read_json()
                if err:
                    self._text(err, 400)
                    return
                with node.lock:
                    if not node.config["sim"]["enabled"]:
                        self._text("set sim.enabled before adding peers", 409)
                        return
                    _peer, perr, code = node.upsert_sim_peer(body, now)
                    if perr:
                        self._text(perr, code)
                        return
                self._text("ok")
                return

            if path == "/api/sim/clear":
                self._read_body()
                with node.lock:
                    node.clear_sim_peers()
                self._text("cleared")
                return

            if path == "/api/system/reboot":
                self._read_body()
                # Answer first; reboot after a short delay so the response makes
                # it out, exactly like the firmware.
                self._text("rebooting")
                threading.Timer(REBOOT_DELAY_MS / 1000.0, node.reboot).start()
                return

            self._text("Not found", 404)

        def do_DELETE(self):
            parsed = urlparse(self.path)
            path, query = parsed.path, parse_qs(parsed.query)
            node.advance()
            if path == "/api/log":
                # Empties the ring. `total`, `warnings` and `errors` survive on
                # purpose: they are since-boot counts, and a client's cursor
                # must never walk backwards.
                with node.lock:
                    node.clear_log()
                self._text("cleared")
                return
            if path == "/api/sim/peer":
                if "uid" not in query:
                    self._text("uid required", 400)
                    return
                uid = parse_uid(query["uid"][0]) or 0
                with node.lock:
                    removed = node.delete_sim_peer(uid)
                self._text("removed" if removed else "no such peer",
                           200 if removed else 404)
                return
            self._text("Not found", 404)

        # -- firmware upload ------------------------------------------------

        def _handle_update(self):
            """Mirrors handleFileUploadData()/handleFileUploadResponse(): the
            upload is accepted only for a .bin (or, on ESP8266, .bin.gz), and
            the response body is the result string the UI shows."""
            raw = self._read_body()
            if not raw:
                self._text("upload failed", 500)
                return
            match = re.search(rb'filename="([^"]*)"', raw[:4096])
            filename = match.group(1).decode("utf-8", "replace") if match else ""
            if not (filename.endswith(".bin") or filename.endswith(".bin.gz")):
                self._text("must upload .bin or .bin.gz", 400)
                return
            self._text("update complete, rebooting")
            threading.Timer(REBOOT_DELAY_MS / 1000.0, node.reboot).start()

        # -- static ---------------------------------------------------------

        def _serve_static(self, path):
            if path == "/" or path.endswith("/"):
                path = path + "index.html"
            root = STATIC_DIR.resolve()
            fs_path = (root / path.lstrip("/")).resolve()
            # Resolve-and-check keeps requests confined to html/ even if a path
            # contains ../. This only ever runs against trusted local content,
            # but there is no reason to skip the check.
            if fs_path != root and not fs_path.is_relative_to(root):
                self._text("Not found", 404)
                return
            try:
                data = fs_path.read_bytes()
            except (FileNotFoundError, IsADirectoryError, PermissionError, OSError):
                self._text("Not found", 404)
                return

            ctype = CONTENT_TYPES.get(fs_path.suffix.lower(), "application/octet-stream")
            if fs_path.suffix.lower() in (".js", ".mjs"):
                try:
                    text = data.decode("utf-8")
                except UnicodeDecodeError:
                    text = None
                if text is not None:
                    data = ENDPOINT_PREFIX_RE.sub(
                        'const ENDPOINT_PREFIX = "";', text).encode("utf-8")
            self._send(data, ctype)

    return Handler


def build_server(port=0, host="127.0.0.1", node=None, config_path=None, quiet=True):
    """Returns an unstarted ThreadingHTTPServer. `port=0` picks a free port, which
    is what the tests use."""
    node = node if node is not None else MockNode()
    server = ThreadingHTTPServer((host, port), make_handler(node, config_path, quiet))
    server.node = node
    server.daemon_threads = True
    return server


def main():
    parser = argparse.ArgumentParser(
        description="Mock FormationFlight v2 device backend (docs/v2-web-api.md).")
    parser.add_argument("--port", type=int, default=8731)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--sim", action="store_true",
                        help="start with sim.enabled, so /api/sim/* works immediately")
    parser.add_argument("--seed", type=int, default=1337,
                        help="RNG seed for RSSI jitter (deterministic by default)")
    parser.add_argument("--no-pmic", action="store_true",
                        help="board with no power-management IC: status sends no "
                             "`power` object at all, which the UI must read as "
                             "'no PMIC' rather than as zero volts")
    parser.add_argument("--on-battery", action="store_true",
                        help="running off the battery instead of USB")
    parser.add_argument("--config-file", default=None,
                        help="also write /api/config/save results to this file")
    parser.add_argument("--verbose", action="store_true", help="log every request")
    args = parser.parse_args()

    node = MockNode(seed=args.seed, power_present=not args.no_pmic,
                    power_usb=not args.on_battery)
    if args.sim:
        node.config["sim"]["enabled"] = True
        node.saved_config["sim"]["enabled"] = True
        # Something to look at straight away.
        node.upsert_sim_peer({"uid": "5eed0001", "name": "SIM1", "mode": "hex",
                              "lat": HOME_LAT, "lon": HOME_LON, "alt_m": 120,
                              "speed_ms": 15, "course_deg": 90, "radius_m": 150}, 0)

    server = build_server(args.port, args.host, node, args.config_file, not args.verbose)
    url = f"http://{args.host}:{server.server_address[1]}/"
    print(f"Serving {STATIC_DIR} and the v2 API at {url} (Ctrl+C to stop)")
    print(f"  node uid {uid_str(NODE_UID)}  sim {'on' if node.config['sim']['enabled'] else 'off'}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nstopping")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
