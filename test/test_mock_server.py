#!/usr/bin/env python3
"""Tests for scripts/mock_server.py, the dev-only backend that lets html/ be
developed against docs/v2-web-api.md with no hardware attached.

Three things are being checked, and only the first is really about Python:

  1. The shared cross-language fixture. Every case in
     docs/spec/fixtures/follow-config-cases.json is run against the mock's
     Follow validation -- the same cases the C++ validator
     (test/test_follow/test_cross_mirror_fixture.cpp) and the JS one
     (test/follow-logic.test.js) are run against. If the three drift apart, a
     config the UI accepts gets rejected by the firmware, on hardware, in a
     field, which is exactly what this mock exists to prevent.

  2. The config contract: configValidate()'s rules and error strings, and
     configMergeJson()'s partial-merge semantics (a one-key POST changes one
     key; a rejected POST changes nothing; the redaction placeholder means
     "leave this secret alone").

  3. The wire shape of /api/status, /api/frames and /api/gnss against what
     docs/v2-web-api.md documents, over a real HTTP server on a real socket.

Stdlib unittest only (the project has no Python test infrastructure to build
on) -- run with:

    python3 test/test_mock_server.py
"""
import copy
import json
import os
import re
import sys
import threading
import time
import unittest
import urllib.error
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))

from mock_server import (  # noqa: E402
    config_to_json,
    FRAME_LOG_CAPACITY,
    FRAME_RESULTS,
    GNSS_BAUD,
    GNSS_LEGACY_IDS,
    GNSS_NAV_RATE_MS,
    GNSS_SEEN_TYPES,
    GNSS_SNIFF_BYTES,
    HEADING_MODE_NAMES,
    LOG_CAPACITY,
    LOG_LEVELS,
    LOG_TEXT_LEN,
    LORA_MODULATION,
    LORA_TX_DEFER_EVERY_MS,
    MAX_NAME_LEN,
    MAX_PASSPHRASE_LEN,
    REDACTED_SECRET,
    RESET_REASON_POWERON,
    RESET_REASON_SOFTWARE,
    UBX_CLASS_ACK,
    UBX_CLASS_NAV,
    UBX_ID_ACK_NAK,
    UBX_ID_NAV_PVT,
    MockNode,
    build_server,
    default_config,
    merge_config,
    redact_config,
    validate_config,
    validate_follow_config,
)

FIXTURE_PATH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..",
    "docs", "spec", "fixtures", "follow-config-cases.json",
)

UID_RE = re.compile(r"^[0-9a-f]{8}$")
# A UBX class and id, as /api/gnss publishes them: two lower-case hex bytes.
GNSS_MSG_RE = re.compile(r"^[0-9a-f]{2}:[0-9a-f]{2}$")
LOWER_HEX_RE = re.compile(r"^[0-9a-f]*$")


def ubx_frames_in(raw):
    """Every complete, checksum-valid UBX frame in a raw window, as
    [(class, id, payload)].

    Resynchronises on the 0xB5 0x62 sync pair the way the firmware's parser
    walks the byte stream: the tap is a ring, so the window starts part-way
    through a frame and the leading bytes are expected to be a partial one.
    """
    frames = []
    i = 0
    while i + 8 <= len(raw):
        if raw[i] != 0xB5 or raw[i + 1] != 0x62:
            i += 1
            continue
        length = int.from_bytes(raw[i + 4:i + 6], "little")
        end = i + 6 + length + 2
        if end > len(raw):
            break
        ck_a = ck_b = 0
        for b in raw[i + 2:i + 6 + length]:
            ck_a = (ck_a + b) & 0xFF
            ck_b = (ck_b + ck_a) & 0xFF
        if (ck_a, ck_b) != (raw[end - 2], raw[end - 1]):
            i += 1
            continue
        frames.append((raw[i + 2], raw[i + 3], raw[i + 6:i + 6 + length]))
        i = end
    return frames


def follow_from_fixture(baseline, overrides):
    """Builds a Follow config block from a fixture case.

    The fixture keeps the v1 key name `targetPeer`; in v2 that field is
    `follow.targetUid` (a 32-bit peer UID, 0 = nearest peer). The C++ mirror
    does the same remap in test_cross_mirror_fixture.cpp's configFromJson(),
    so all three validators see the same case.
    """
    cfg = dict(default_config()["follow"])
    for src in (baseline, overrides):
        for key, value in src.items():
            if key == "targetPeer":
                cfg["targetUid"] = value
            else:
                cfg[key] = value
    return cfg


# ---------------------------------------------------------------------------
# 1. The shared cross-language fixture
# ---------------------------------------------------------------------------

class FollowFixtureTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(FIXTURE_PATH, encoding="utf-8") as f:
            cls.fixture = json.load(f)

    def test_fixture_has_cases(self):
        self.assertGreater(len(self.fixture["cases"]), 0)

    def test_validate_follow_config_matches_every_fixture_case(self):
        for tc in self.fixture["cases"]:
            with self.subTest(case=tc["name"]):
                cfg = follow_from_fixture(self.fixture["baseline"], tc["overrides"])
                err = validate_follow_config(cfg)
                self.assertEqual(
                    err is None, tc["expectValid"],
                    f'case "{tc["name"]}": expected expectValid={tc["expectValid"]}, '
                    f'got {err is None} (err={err!r})')

    def test_whole_config_validation_matches_every_fixture_case(self):
        """The same cases through validate_config(), i.e. the path a POST takes:
        configValidate() delegates to followValidateConfig(), so the verdict and
        the message must survive the trip."""
        for tc in self.fixture["cases"]:
            with self.subTest(case=tc["name"]):
                cfg = default_config()
                cfg["follow"] = follow_from_fixture(self.fixture["baseline"], tc["overrides"])
                err = validate_config(cfg)
                self.assertEqual(
                    err is None, tc["expectValid"],
                    f'case "{tc["name"]}": expected expectValid={tc["expectValid"]}, '
                    f'got {err is None} (err={err!r})')

    def test_fixture_baseline_matches_the_firmware_defaults(self):
        """Every baseline value that still exists as a Follow field should equal
        the compile-time default in lib/ff_core/follow.h. A drift here means the
        fixture is testing a config nobody ships."""
        defaults = default_config()["follow"]
        for key, value in self.fixture["baseline"].items():
            if key == "targetPeer":
                key = "targetUid"
            self.assertIn(key, defaults, f"fixture baseline key {key} is not a Follow field")
            self.assertAlmostEqual(
                float(defaults[key]), float(value), places=6,
                msg=f"fixture baseline {key}={value} but the firmware default is {defaults[key]}")


# ---------------------------------------------------------------------------
# 2a. Defaults
# ---------------------------------------------------------------------------

class DefaultConfigTest(unittest.TestCase):
    def test_defaults_validate(self):
        self.assertIsNone(validate_config(default_config()))

    def test_defaults_mirror_config_h(self):
        cfg = default_config()
        self.assertEqual(cfg["version"], 1)
        self.assertEqual(cfg["node"], {"name": "", "listen_only": False})
        self.assertEqual(cfg["security"], {"passphrase": ""})
        self.assertEqual(cfg["rate"], {"target_load": 0.15, "min_interval_ms": 100,
                                       "max_interval_ms": 1000, "jitter_frac": 0.25,
                                       "duty_cycle_pct": 0})
        self.assertEqual(cfg["peers"], {"timeout_ms": 6000, "announce_interval_ms": 2000})
        self.assertEqual(cfg["msp"], {"radar_interval_ms": 100})
        self.assertEqual(cfg["gnss"], {"rate_hz": 10})
        self.assertEqual(cfg["radios"], {"espnow_enabled": True, "lora_enabled": True,
                                         "lora_power_dbm": 0})
        self.assertEqual(cfg["wifi"], {"ap": True, "ssid": "", "psk": "", "ap_psk": "",
                                       "channel": 1})
        self.assertEqual(cfg["sim"], {"enabled": False})

    def test_follow_defaults_mirror_follow_h(self):
        f = default_config()["follow"]
        self.assertEqual(f["ofsLongM"], -15.0)
        self.assertEqual(f["ofsLatM"], 0.0)
        self.assertEqual(f["ofsVertM"], 10.0)
        self.assertEqual(f["targetUid"], 0)
        self.assertEqual(f["emitHz"], 4)
        self.assertEqual(f["peerTimeoutMs"], 1500)
        self.assertEqual(f["minSepM"], 8.0)
        self.assertEqual(f["minVSepM"], 13.0)
        self.assertEqual(f["maxTargetDistM"], 50.0)
        self.assertEqual(f["minAltM"], 3.0)
        self.assertEqual(f["minCourseSpeed"], 2.0)
        self.assertEqual(f["headingMode"], "POINT_LEADER")
        self.assertEqual(f["triggerMode"], "GCSNAV")
        self.assertEqual(f["headingDeg"], 0.0)
        for key in ("statusGvarIndex", "conditionFlagsGvarIndex", "rcLongChannel",
                    "rcLatChannel", "rcVertChannel", "targetSpeedGvarIndex",
                    "autothrottleEngageGvarIndex", "autothrottleEnableRcChannel"):
            self.assertEqual(f[key], -1, key)
        self.assertEqual(f["autothrottleEnableMinThresholdUs"], 1700)
        self.assertEqual(f["autothrottleEnableMaxThresholdUs"], 2100)
        self.assertEqual(f["speedCorrectionAccelCmS2"], 0)
        self.assertEqual(f["minTargetSpeedMps"], 0.0)
        self.assertEqual(f["maxTargetSpeedMps"], 0.0)
        self.assertIs(f["debug"], False)


# ---------------------------------------------------------------------------
# 2b. configValidate()'s own rules
# ---------------------------------------------------------------------------

class ConfigValidateTest(unittest.TestCase):
    def check(self, mutate, expected_err):
        cfg = default_config()
        mutate(cfg)
        self.assertEqual(validate_config(cfg), expected_err)

    def test_target_load_bounds(self):
        self.check(lambda c: c["rate"].update(target_load=0.0),
                   "rate.target_load must be in (0, 1]")
        self.check(lambda c: c["rate"].update(target_load=1.5),
                   "rate.target_load must be in (0, 1]")
        self.check(lambda c: c["rate"].update(target_load=1.0), None)

    def test_jitter_frac_bounds(self):
        self.check(lambda c: c["rate"].update(jitter_frac=-0.1),
                   "rate.jitter_frac must be in [0, 1)")
        self.check(lambda c: c["rate"].update(jitter_frac=1.0),
                   "rate.jitter_frac must be in [0, 1)")
        self.check(lambda c: c["rate"].update(jitter_frac=0.0), None)

    def test_interval_bounds(self):
        self.check(lambda c: c["rate"].update(min_interval_ms=0),
                   "rate.min_interval_ms must be > 0")
        self.check(lambda c: c["rate"].update(max_interval_ms=50),
                   "rate.max_interval_ms must be >= rate.min_interval_ms")

    def test_peer_and_msp_intervals(self):
        self.check(lambda c: c["peers"].update(timeout_ms=0),
                   "peers.timeout_ms must be > 0")
        self.check(lambda c: c["peers"].update(announce_interval_ms=0),
                   "peers.announce_interval_ms must be > 0")
        self.check(lambda c: c["msp"].update(radar_interval_ms=0),
                   "msp.radar_interval_ms must be > 0")

    def test_gnss_rate(self):
        self.check(lambda c: c["gnss"].update(rate_hz=0), "gnss.rate_hz must be 1-25")
        self.check(lambda c: c["gnss"].update(rate_hz=26), "gnss.rate_hz must be 1-25")
        self.check(lambda c: c["gnss"].update(rate_hz=25), None)

    def test_at_least_one_radio(self):
        def kill_radios(c):
            c["radios"].update(espnow_enabled=False, lora_enabled=False)
        self.check(kill_radios, "at least one radio must be enabled")

    def test_sim_counts_as_a_radio(self):
        cfg = default_config()
        cfg["radios"].update(espnow_enabled=False, lora_enabled=False)
        cfg["sim"]["enabled"] = True
        self.assertIsNone(validate_config(cfg))

    def test_lora_power(self):
        msg = "radios.lora_power_dbm must be 0 (target default) or 1-30"
        self.check(lambda c: c["radios"].update(lora_power_dbm=-1), msg)
        self.check(lambda c: c["radios"].update(lora_power_dbm=31), msg)
        self.check(lambda c: c["radios"].update(lora_power_dbm=30), None)

    def test_duty_cycle_pct(self):
        """A legal limit, not a preference: the rate controller treats it as a
        hard floor on the beacon interval, so it has to survive validation."""
        for value, ok in ((0, True), (1, True), (10, True), (100, True), (101, False)):
            cfg = default_config()
            cfg["rate"]["duty_cycle_pct"] = value
            err = validate_config(cfg)
            if ok:
                self.assertIsNone(err, f"duty_cycle_pct={value} should be accepted")
            else:
                self.assertEqual(err, "rate.duty_cycle_pct must be 0 (no limit) or 1-100")

    def test_wifi_channel(self):
        """ESP-NOW shares the WiFi radio, so this is the channel the whole mesh
        has to agree on. 14 is Japan-only and the ESP will not take it."""
        msg = "wifi.channel must be 1-13"
        self.check(lambda c: c["wifi"].update(channel=0), msg)
        self.check(lambda c: c["wifi"].update(channel=14), msg)
        self.check(lambda c: c["wifi"].update(channel=1), None)
        self.check(lambda c: c["wifi"].update(channel=13), None)

    def test_wifi_station_needs_an_ssid(self):
        self.check(lambda c: c["wifi"].update(ap=False),
                   "wifi.ssid is required when wifi.ap is false")
        self.check(lambda c: c["wifi"].update(ap=False, ssid="home"), None)

    def test_ap_psk_length(self):
        msg = "wifi.ap_psk must be empty (open) or at least 8 characters"
        self.check(lambda c: c["wifi"].update(ap_psk="short"), msg)
        self.check(lambda c: c["wifi"].update(ap_psk="longenough"), None)
        self.check(lambda c: c["wifi"].update(ap_psk=""), None)

    def test_follow_errors_surface_through_config_validation(self):
        self.check(lambda c: c["follow"].update(emitHz=0), "emitHz must be > 0")


# ---------------------------------------------------------------------------
# 2c. The partial-merge contract
# ---------------------------------------------------------------------------

class MergeConfigTest(unittest.TestCase):
    def test_one_key_post_does_not_wipe_anything_else(self):
        cfg = default_config()
        cfg["wifi"].update(ssid="fieldbench", psk="supersecret", ap_psk="alsosecret")
        cfg["security"]["passphrase"] = "groupkey"
        merged, err = merge_config({"follow": {"ofsLongM": -20}}, cfg)
        self.assertIsNone(err)
        self.assertEqual(merged["follow"]["ofsLongM"], -20.0)
        # Everything else survives, including the secrets the POST never mentioned.
        self.assertEqual(merged["wifi"]["ssid"], "fieldbench")
        self.assertEqual(merged["wifi"]["psk"], "supersecret")
        self.assertEqual(merged["wifi"]["ap_psk"], "alsosecret")
        self.assertEqual(merged["security"]["passphrase"], "groupkey")
        self.assertEqual(merged["follow"]["ofsVertM"], cfg["follow"]["ofsVertM"])

    def test_a_partial_section_only_touches_the_keys_it_names(self):
        cfg = default_config()
        merged, err = merge_config({"rate": {"target_load": 0.2}}, cfg)
        self.assertIsNone(err)
        self.assertEqual(merged["rate"]["target_load"], 0.2)
        self.assertEqual(merged["rate"]["min_interval_ms"], 100)
        self.assertEqual(merged["rate"]["max_interval_ms"], 1000)
        self.assertEqual(merged["rate"]["jitter_frac"], 0.25)

    def test_rejected_post_changes_nothing(self):
        cfg = default_config()
        before = copy.deepcopy(cfg)
        merged, err = merge_config(
            {"node": {"name": "NEWNAME"}, "follow": {"emitHz": 0}}, cfg)
        self.assertIsNone(merged)
        self.assertEqual(err, "emitHz must be > 0")
        # The caller's config is untouched -- not even the valid half applied.
        self.assertEqual(cfg, before)

    def test_unknown_keys_are_ignored(self):
        """A newer UI against older firmware degrades rather than breaks."""
        merged, err = merge_config(
            {"future_section": {"x": 1}, "follow": {"notAFieldYet": 7, "emitHz": 8}},
            default_config())
        self.assertIsNone(err)
        self.assertEqual(merged["follow"]["emitHz"], 8)
        self.assertNotIn("notAFieldYet", merged["follow"])
        self.assertNotIn("future_section", merged)

    def test_version_is_not_mergeable(self):
        merged, err = merge_config({"version": 99}, default_config())
        self.assertIsNone(err)
        self.assertEqual(merged["version"], 1)

    def test_trigger_mode_is_read_only(self):
        merged, err = merge_config({"follow": {"triggerMode": "AUX"}}, default_config())
        self.assertIsNone(err)
        self.assertEqual(merged["follow"]["triggerMode"], "GCSNAV")

    def test_heading_mode_accepts_known_names_and_ignores_the_rest(self):
        for name in HEADING_MODE_NAMES:
            merged, err = merge_config({"follow": {"headingMode": name}}, default_config())
            self.assertIsNone(err)
            self.assertEqual(merged["follow"]["headingMode"], name)
        merged, err = merge_config({"follow": {"headingMode": "SIDEWAYS"}}, default_config())
        self.assertIsNone(err)
        self.assertEqual(merged["follow"]["headingMode"], "POINT_LEADER")

    def test_strings_truncate_rather_than_overflow(self):
        merged, err = merge_config({"node": {"name": "A" * 40}}, default_config())
        self.assertIsNone(err)
        self.assertEqual(len(merged["node"]["name"]), MAX_NAME_LEN)
        merged, err = merge_config({"security": {"passphrase": "p" * 100}}, merged)
        self.assertIsNone(err)
        self.assertEqual(len(merged["security"]["passphrase"]), MAX_PASSPHRASE_LEN)

    def test_redaction_placeholder_leaves_the_secret_alone(self):
        cfg = default_config()
        cfg["security"]["passphrase"] = "groupkey"
        cfg["wifi"].update(psk="stationpsk", ap_psk="appassword")
        merged, err = merge_config(
            {"security": {"passphrase": REDACTED_SECRET},
             "wifi": {"psk": REDACTED_SECRET, "ap_psk": REDACTED_SECRET}}, cfg)
        self.assertIsNone(err)
        self.assertEqual(merged["security"]["passphrase"], "groupkey")
        self.assertEqual(merged["wifi"]["psk"], "stationpsk")
        self.assertEqual(merged["wifi"]["ap_psk"], "appassword")

    def test_the_redacted_document_round_trips(self):
        """The UI posts back the document it was given; nothing may be lost."""
        cfg = default_config()
        cfg["security"]["passphrase"] = "groupkey"
        cfg["wifi"].update(ssid="fieldbench", psk="stationpsk", ap_psk="appassword")
        merged, err = merge_config(redact_config(cfg), cfg)
        self.assertIsNone(err)
        self.assertEqual(merged, cfg)

    def test_a_secret_can_still_be_changed_and_cleared(self):
        cfg = default_config()
        cfg["security"]["passphrase"] = "groupkey"
        merged, err = merge_config({"security": {"passphrase": "newkey"}}, cfg)
        self.assertIsNone(err)
        self.assertEqual(merged["security"]["passphrase"], "newkey")
        merged, err = merge_config({"security": {"passphrase": ""}}, merged)
        self.assertIsNone(err)
        self.assertEqual(merged["security"]["passphrase"], "")

    def test_redact_config_hides_set_secrets_only(self):
        cfg = default_config()
        cfg["security"]["passphrase"] = "groupkey"
        red = redact_config(cfg)
        self.assertEqual(red["security"]["passphrase"], REDACTED_SECRET)
        # Unset secrets come back as "", not as the placeholder.
        self.assertEqual(red["wifi"]["psk"], "")
        self.assertEqual(red["wifi"]["ap_psk"], "")
        # The original is untouched.
        self.assertEqual(cfg["security"]["passphrase"], "groupkey")


# ---------------------------------------------------------------------------
# 3. The HTTP surface
# ---------------------------------------------------------------------------

class ApiTestCase(unittest.TestCase):
    """Base: one real server on a real socket per test class."""

    @classmethod
    def setUpClass(cls):
        cls.server = build_server(port=0)
        cls.node = cls.server.node
        cls.base = f"http://127.0.0.1:{cls.server.server_address[1]}"
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=5)

    def request(self, method, path, body=None, ctype="application/json"):
        data = None
        if body is not None:
            data = body if isinstance(body, bytes) else json.dumps(body).encode("utf-8")
        req = urllib.request.Request(self.base + path, data=data, method=method)
        if data is not None:
            req.add_header("Content-Type", ctype)
        try:
            with urllib.request.urlopen(req, timeout=10) as resp:
                return resp.status, resp.read().decode("utf-8"), resp.headers
        except urllib.error.HTTPError as exc:
            return exc.code, exc.read().decode("utf-8"), exc.headers

    def get_json(self, path):
        status, body, _ = self.request("GET", path)
        self.assertEqual(status, 200, body)
        return json.loads(body)


class StatusShapeTest(ApiTestCase):
    def test_status_top_level_shape(self):
        doc = self.get_json("/api/status")
        for key in ("node", "location", "radios", "peers", "crypto", "stats", "fc",
                    "follow", "sim", "wifi", "system", "loop", "log",
                    "reboot_required", "config_corrupt"):
            self.assertIn(key, doc)
        self.assertIsInstance(doc["reboot_required"], bool)
        self.assertIsInstance(doc["config_corrupt"], bool)

    def test_node_block(self):
        node = self.get_json("/api/status")["node"]
        for key in ("uid", "name", "version", "uptime_ms", "free_heap", "listen_only"):
            self.assertIn(key, node)
        self.assertRegex(node["uid"], UID_RE)
        self.assertLessEqual(len(node["name"]), MAX_NAME_LEN)
        self.assertIsInstance(node["uptime_ms"], int)
        self.assertIsInstance(node["listen_only"], bool)

    def test_location_block_uses_1e7_degrees(self):
        loc = self.get_json("/api/status")["location"]
        for key in ("valid", "source", "lat", "lon", "alt_m", "speed_cms",
                    "course_ddeg", "armed"):
            self.assertIn(key, loc)
        self.assertIsInstance(loc["lat"], int)
        self.assertIsInstance(loc["lon"], int)
        self.assertGreater(abs(loc["lat"]), 1000000)  # 1e7 degrees, not plain degrees
        self.assertLessEqual(loc["course_ddeg"], 3599)

    def test_location_reports_fix_quality(self):
        """A fix is not a fix: "no fix" and "four satellites wandering by 30 m"
        look identical without these."""
        loc = self.get_json("/api/status")["location"]
        self.assertIsInstance(loc["sats"], int)
        self.assertGreaterEqual(loc["sats"], 0)
        # UBX NAV-PVT numbering, the one scheme every source reports: 0 none,
        # 1 dead reckoning, 2 2D, 3 3D, 4 3D + dead reckoning, 5 time only.
        # The MSP path converts its own 0/1/2 on the way in, so a healthy mock
        # fix is 3.
        self.assertIn(loc["fix_type"], (0, 1, 2, 3, 4, 5))
        self.assertEqual(loc["fix_type"], 3)
        # A float, because the firmware divides the x100 wire value.
        self.assertIsInstance(loc["hdop"], float)
        self.assertGreater(loc["hdop"], 0.0)

    def test_hdop_is_absent_rather_than_zero(self):
        """0.00 is a flawless fix, so it cannot double as "this source does not
        report HDOP". The firmware omits the field instead."""
        try:
            with self.node.lock:
                self.node.hdop_x100 = 0
            self.assertNotIn("hdop", self.get_json("/api/status")["location"])
        finally:
            with self.node.lock:
                self.node.hdop_x100 = 131

    def test_radio_blocks(self):
        radios = self.get_json("/api/status")["radios"]
        self.assertGreaterEqual(len(radios), 1)
        for radio in radios:
            for key in ("index", "name", "enabled", "sim", "tx", "rx_ok",
                        "rx_crypto_fail", "rx_replay", "rx_decode_fail", "rx_self",
                        "last_rssi", "last_rx_age_ms", "beacon_interval_ms",
                        "airtime_ms", "peers", "rx_dropped", "tx_dropped",
                        "tx_deferred", "tx_timeouts"):
                self.assertIn(key, radio, radio.get("name"))
            self.assertIsInstance(radio["sim"], bool)
            self.assertIsInstance(radio["enabled"], bool)
            # Frames lost inside the node. Always present, even at zero: the UI
            # has to be able to tell "none dropped" from "this firmware does not
            # report it".
            self.assertIsInstance(radio["rx_dropped"], int)
            self.assertIsInstance(radio["tx_dropped"], int)
            self.assertGreaterEqual(radio["rx_dropped"], 0)
            self.assertGreaterEqual(radio["tx_dropped"], 0)
            # False only for a receive-only driver, which the simulated radio is.
            self.assertIsInstance(radio["transmits"], bool)

    def test_transmit_counters_are_present_on_every_radio(self):
        """tx_deferred and tx_timeouts are emitted unconditionally, zero-filled
        for the drivers that do not track them, so the UI can tell "none" from
        "this firmware does not report it"."""
        radios = {r["name"]: r for r in self.get_json("/api/status")["radios"]}
        for radio in radios.values():
            self.assertIsInstance(radio["tx_deferred"], int, radio["name"])
            self.assertIsInstance(radio["tx_timeouts"], int, radio["name"])
            self.assertGreaterEqual(radio["tx_deferred"], 0, radio["name"])
            self.assertGreaterEqual(radio["tx_timeouts"], 0, radio["name"])
        # ESP-NOW hands the frame to the MAC: no one-frame slot to defer into
        # and no transmit-done interrupt to miss, so its driver reports neither.
        self.assertEqual(radios["ESPNOW"]["tx_deferred"], 0)
        self.assertEqual(radios["ESPNOW"]["tx_timeouts"], 0)

    def test_modulation_is_present_on_lora_and_absent_everywhere_else(self):
        """Read back from the driver, so only a driver with a frequency has one.
        ESP-NOW's Info is all zeros and the virtual radio never keyed an
        antenna, so neither sends the object -- absent, not zero-filled."""
        radios = {r["name"]: r for r in self.get_json("/api/status")["radios"]}
        self.assertIn("LORA", radios)
        mod = radios["LORA"]["modulation"]
        self.assertEqual(set(mod), {"frequency_hz", "bandwidth_khz", "spreading_factor",
                                    "coding_rate", "power_dbm"})
        # The 915 band this project ships (platformio.ini's env_common_915).
        self.assertEqual(mod["frequency_hz"], 920000000)
        self.assertEqual(mod["bandwidth_khz"], 500.0)
        self.assertEqual(mod["spreading_factor"], 8)
        self.assertEqual(mod["coding_rate"], 7)   # a denominator: 4/7
        self.assertEqual(mod, LORA_MODULATION)
        # power_dbm is the driver's own setting (the LORA_POWER build flag), not
        # radios.lora_power_dbm, and may legitimately differ from the config.
        self.assertIsInstance(mod["power_dbm"], int)

        self.assertNotIn("modulation", radios["ESPNOW"])
        # SNR is a LoRa-only reading, so ESP-NOW never carries one either. 0 dB
        # is an ordinary SNR, so it could not have doubled as "no reading".
        self.assertNotIn("last_snr_db", radios["ESPNOW"])
        if "last_snr_db" in radios["LORA"]:
            self.assertIsInstance(radios["LORA"]["last_snr_db"], float)

    def test_system_block(self):
        sys_doc = self.get_json("/api/status")["system"]
        for key in ("cpu_mhz", "free_heap", "sketch_size", "free_sketch_space",
                    "reset_reason", "largest_free_block", "flash_size"):
            self.assertIn(key, sys_doc)
        # Why the node last restarted: the most useful field here, and invisible
        # everywhere else in the API.
        self.assertEqual(sys_doc["reset_reason"], RESET_REASON_POWERON)
        self.assertIsInstance(sys_doc["reset_reason"], str)
        # One contiguous block is never more than the heap it sits in, which is
        # the whole point of publishing it next to free_heap.
        self.assertLessEqual(sys_doc["largest_free_block"], sys_doc["free_heap"])
        self.assertLessEqual(sys_doc["min_free_heap"], sys_doc["free_heap"])
        self.assertTrue(0 <= sys_doc["heap_fragmentation_pct"] <= 100)
        self.assertGreater(sys_doc["flash_size"], sys_doc["sketch_size"])
        # fillSystem() and the node block are the same ESP.getFreeHeap() call.
        self.assertEqual(sys_doc["free_heap"], self.get_json("/api/status")["node"]["free_heap"])

    def test_loop_block(self):
        loop = self.get_json("/api/status")["loop"]
        self.assertEqual(set(loop), {"last_us", "min_us", "max_us", "mean_us", "rate_hz",
                                     "samples", "overruns", "overrun_threshold_us"})
        for key, value in loop.items():
            self.assertIsInstance(value, int, key)
            self.assertGreaterEqual(value, 0, key)
        self.assertLessEqual(loop["min_us"], loop["mean_us"])
        self.assertLessEqual(loop["mean_us"], loop["max_us"])
        # rateHz() is derived from the mean, not measured separately.
        self.assertEqual(loop["rate_hz"], 1000000 // loop["mean_us"])
        # Set at boot from the shortest beacon interval (rate.min_interval_ms).
        self.assertEqual(loop["overrun_threshold_us"], 100 * 1000)

    def test_log_counters_block(self):
        """The status document carries the counters only; the lines themselves
        are GET /api/log."""
        log = self.get_json("/api/status")["log"]
        self.assertEqual(set(log), {"total", "warnings", "errors"})
        for key, value in log.items():
            self.assertIsInstance(value, int, key)
        self.assertGreater(log["total"], 0)   # the boot lines, at least
        self.assertGreaterEqual(log["total"], log["warnings"] + log["errors"])

    def test_power_block_is_self_consistent(self):
        """A PMIC that answered. Charge and discharge are separate readings, so
        a node on USB with a battery shows a supply voltage and a charge
        current at once -- the state people most often misread."""
        power = self.get_json("/api/status")["power"]
        for key in ("battery_v", "supply_v", "charge_ma", "discharge_ma", "pmic_temp_c",
                    "battery_present", "charging", "usb_present"):
            self.assertIn(key, power)
        for key in ("battery_present", "charging", "usb_present"):
            self.assertIsInstance(power[key], bool)
        self.assertTrue(power["usb_present"])
        self.assertGreater(power["supply_v"], 4.0)
        self.assertTrue(power["charging"])
        self.assertGreater(power["charge_ma"], 0.0)
        self.assertEqual(power["discharge_ma"], 0.0)
        self.assertTrue(0 <= power["battery_pct"] <= 100)
        self.assertIsInstance(power["battery_pct"], int)

    def test_battery_pct_is_absent_when_the_pmic_will_not_estimate_one(self):
        """0% and "no estimate" are very different things to show a pilot."""
        try:
            with self.node.lock:
                self.node.power_estimates_pct = False
            power = self.get_json("/api/status")["power"]
            self.assertNotIn("battery_pct", power)
            # The rest of the object still comes through.
            self.assertIn("battery_v", power)
        finally:
            with self.node.lock:
                self.node.power_estimates_pct = True

    def test_power_is_absent_entirely_without_a_pmic(self):
        """Absence is the diagnostic: on a T-Beam the same chip powers the GPS
        and LoRa rails, so no `power` object means no GPS either. The UI must
        never read it as zero volts."""
        try:
            with self.node.lock:
                self.node.power_present = False
            self.assertNotIn("power", self.get_json("/api/status"))
        finally:
            with self.node.lock:
                self.node.power_present = True

    def test_peer_blocks(self):
        peers = self.get_json("/api/status")["peers"]
        self.assertGreaterEqual(len(peers), 1)
        for peer in peers:
            for key in ("uid", "name", "lat", "lon", "alt_m", "speed_cms",
                        "course_ddeg", "flags", "rssi", "age_ms", "packets",
                        "radios", "distance_m", "bearing_deg", "rel_alt_m"):
                self.assertIn(key, peer, peer.get("name"))
            self.assertRegex(peer["uid"], UID_RE)
            self.assertLessEqual(len(peer["name"]), MAX_NAME_LEN)
            self.assertIsInstance(peer["lat"], int)
            self.assertIsInstance(peer["radios"], list)
            self.assertTrue(0 <= peer["course_ddeg"] <= 3599)
            self.assertLess(peer["rssi"], 0)
            self.assertLessEqual(0, peer["bearing_deg"])
            self.assertLess(peer["bearing_deg"], 360)

    def test_crypto_stats_and_fc_blocks(self):
        doc = self.get_json("/api/status")
        self.assertEqual(set(doc["crypto"]), {"mode", "bad_tag", "replay", "tx_counter"})
        self.assertEqual(set(doc["stats"]),
                         {"beacons_sent", "announces_sent", "rx_ok", "rx_rejected", "rx_self"})
        for key in ("connected", "variant", "version", "platform", "armed", "gcs_nav",
                    "heading_hold"):
            self.assertIn(key, doc["fc"])
        self.assertIn(doc["fc"]["platform"], (0, 1, 2, 3, 4, 5, 255))
        self.assertEqual(set(doc["sim"]), {"enabled", "peers"})

    def test_wifi_block_in_ap_mode(self):
        """Running our own AP, the configured channel is the channel: nobody
        else gets a say, so the two must agree."""
        wifi = self.get_json("/api/status")["wifi"]
        self.assertEqual(set(wifi), {"mode", "channel", "configured_channel", "ap_clients"})
        self.assertEqual(wifi["mode"], "ap")
        self.assertEqual(wifi["channel"], wifi["configured_channel"])
        self.assertTrue(1 <= wifi["channel"] <= 13)
        self.assertIsInstance(wifi["ap_clients"], int)
        # The station keys only exist in ap_sta; there is no station to describe.
        self.assertNotIn("sta_connected", wifi)
        self.assertNotIn("sta_rssi", wifi)

    def test_wifi_block_in_station_mode(self):
        """Joining a network hands the channel to the router, so `channel` and
        `configured_channel` can disagree -- and ESP-NOW follows `channel`,
        which is why the mock makes them disagree on purpose."""
        try:
            status, body, _ = self.request(
                "POST", "/api/config",
                {"wifi": {"ap": False, "ssid": "bench", "channel": 11}})
            self.assertEqual(status, 200, body)
            wifi = self.get_json("/api/status")["wifi"]
            self.assertEqual(wifi["mode"], "ap_sta")
            self.assertEqual(wifi["configured_channel"], 11)
            self.assertNotEqual(wifi["channel"], wifi["configured_channel"])
            self.assertIsInstance(wifi["sta_connected"], bool)
            self.assertLess(wifi["sta_rssi"], 0)
        finally:
            status, body, _ = self.request(
                "POST", "/api/config", {"wifi": {"ap": True, "channel": 1}})
            self.assertEqual(status, 200, body)

    def test_follow_block_and_its_absent_when_unknown_fields(self):
        follow = self.get_json("/api/status")["follow"]
        for key in ("state", "gate_active", "locked_uid", "locked_name", "platform",
                    "autothrottle_armed", "rc_slot_frozen", "prearm_failed"):
            self.assertIn(key, follow)
        self.assertIn(follow["state"], ("IDLE", "ACQUIRING", "LOCKED", "LOCKED_HOLDING"))
        self.assertRegex(follow["locked_uid"], UID_RE)
        # GVAR slots are disabled by default, so those keys are absent rather
        # than present and zero (zero is a legitimate GVAR value).
        self.assertNotIn("status_gvar", follow)
        self.assertNotIn("condition_gvar", follow)
        # No RC axis assigned by default, so no pre-arm candidate offset either.
        self.assertNotIn("prearm_offset", follow)
        # No target yet at this uptime, so nothing that is only meaningful
        # alongside one is present either.
        if "target" not in follow:
            for key in ("live_offset", "autothrottle_engaged", "target_speed_cms"):
                self.assertNotIn(key, follow)

    def test_status_moves(self):
        """The whole point of the mock: the numbers change on their own."""
        first = self.get_json("/api/status")
        time.sleep(0.2)
        second = self.get_json("/api/status")
        self.assertGreater(second["node"]["uptime_ms"], first["node"]["uptime_ms"])
        moved = [(a["lat"], a["lon"]) != (b["lat"], b["lon"])
                 for a, b in zip(first["peers"], second["peers"])]
        self.assertTrue(all(moved), "every peer should have moved")
        self.assertGreater(second["crypto"]["tx_counter"], 0)


class FramesShapeTest(ApiTestCase):
    def test_frames_shape(self):
        doc = self.get_json("/api/frames")
        self.assertEqual(set(doc), {"total", "capacity", "frames"})
        self.assertEqual(doc["capacity"], FRAME_LOG_CAPACITY)
        self.assertLessEqual(len(doc["frames"]), FRAME_LOG_CAPACITY)
        self.assertGreaterEqual(len(doc["frames"]), 1)
        for frame in doc["frames"]:
            self.assertEqual(set(frame),
                             {"ms", "radio", "uid", "type", "len", "rssi", "result"})
            self.assertRegex(frame["uid"], UID_RE)
            self.assertIn(frame["result"], FRAME_RESULTS)
            self.assertIn(frame["type"], (0, 1, 2))
            self.assertIsInstance(frame["len"], int)

    def test_frames_are_newest_first(self):
        frames = self.get_json("/api/frames")["frames"]
        stamps = [f["ms"] for f in frames]
        self.assertEqual(stamps, sorted(stamps, reverse=True))

    def test_since_returns_only_what_the_client_missed(self):
        first = self.get_json("/api/frames")
        total = first["total"]
        # Everything up to `total` has been seen, so only what arrived since
        # then comes back -- never more than the counter has advanced by.
        again = self.get_json(f"/api/frames?since={total}")
        self.assertLessEqual(len(again["frames"]), max(0, again["total"] - total))
        # A since of 0 is the whole ring.
        self.assertEqual(len(self.get_json("/api/frames?since=0")["frames"]),
                         len(self.get_json("/api/frames")["frames"]))

    def test_since_paging_is_exact(self):
        """Driven directly so the clock can be moved: after N more frames, a
        client that asks for what it missed gets exactly those N."""
        node = MockNode()
        node._t0 -= 5.0
        node.advance()
        before = node.frames_json()
        self.assertGreater(before["total"], 0)
        node._t0 -= 1.0
        node.advance()
        after = node.frames_json(since=before["total"])
        self.assertEqual(len(after["frames"]),
                         after["total"] - before["total"])
        # Asking again with nothing new in between returns nothing.
        self.assertEqual(node.frames_json(since=after["total"])["frames"], [])

    def test_since_must_be_an_integer(self):
        status, body, _ = self.request("GET", "/api/frames?since=soon")
        self.assertEqual(status, 400)
        self.assertIn("since", body)


class LogApiTest(ApiTestCase):
    """GET and DELETE /api/log.

    The trickle is parked and the ring seeded by hand, because every assertion
    here is about the contract -- newest first, `since`, and what a clear does
    and does not reset -- and none of them should be racing the wall clock.
    """

    def setUp(self):
        with self.node.lock:
            self.node.next_log_ms = self.node.uptime_ms() + 3600000
            self.node.clear_log()
            for i in range(5):
                self.node.log_add("info", f"seeded line {i}", 1000 + i)

    def test_log_shape(self):
        doc = self.get_json("/api/log")
        self.assertEqual(set(doc), {"total", "capacity", "warnings", "errors", "entries"})
        self.assertEqual(doc["capacity"], LOG_CAPACITY)
        self.assertLessEqual(len(doc["entries"]), LOG_CAPACITY)
        self.assertGreaterEqual(len(doc["entries"]), 1)
        for entry in doc["entries"]:
            self.assertEqual(set(entry), {"ms", "level", "text"})
            self.assertIn(entry["level"], LOG_LEVELS)
            self.assertIsInstance(entry["ms"], int)
            # Truncation is deliberate: a line long enough to be cut is one that
            # should have been shorter.
            self.assertLessEqual(len(entry["text"]), LOG_TEXT_LEN)

    def test_entries_are_newest_first(self):
        entries = self.get_json("/api/log")["entries"]
        stamps = [e["ms"] for e in entries]
        self.assertEqual(stamps, sorted(stamps, reverse=True))
        self.assertEqual(entries[0]["text"], "seeded line 4")

    def test_since_returns_only_what_the_client_missed(self):
        """Entry `i` has sequence `total - 1 - i`, so a client that polls with
        the `total` it last saw gets exactly what arrived after it."""
        before = self.get_json("/api/log")
        with self.node.lock:
            for i in range(3):
                self.node.log_add("info", f"after the cursor {i}", 2000 + i)
        doc = self.get_json(f"/api/log?since={before['total']}")
        self.assertEqual(doc["total"], before["total"] + 3)
        self.assertEqual(len(doc["entries"]), 3)
        self.assertEqual([e["text"] for e in doc["entries"]],
                         ["after the cursor 2", "after the cursor 1", "after the cursor 0"])
        # Asking again with nothing new in between returns nothing at all.
        self.assertEqual(self.get_json(f"/api/log?since={doc['total']}")["entries"], [])

    def test_since_zero_is_the_whole_ring(self):
        self.assertEqual(self.get_json("/api/log?since=0")["entries"],
                         self.get_json("/api/log")["entries"])

    def test_since_must_be_an_integer(self):
        status, body, _ = self.request("GET", "/api/log?since=soon")
        self.assertEqual(status, 400)
        self.assertIn("since", body)

    def test_the_ring_rolls_and_total_outruns_capacity(self):
        with self.node.lock:
            for i in range(LOG_CAPACITY + 10):
                self.node.log_add("info", f"rolling {i}", 3000 + i)
        doc = self.get_json("/api/log")
        self.assertEqual(len(doc["entries"]), LOG_CAPACITY)
        self.assertGreater(doc["total"], doc["capacity"])
        # The oldest lines are gone, the newest are not.
        self.assertEqual(doc["entries"][0]["text"], f"rolling {LOG_CAPACITY + 9}")

    def test_delete_empties_the_ring_but_never_the_counters(self):
        """A clear that reset `total` would send every outstanding client cursor
        backwards, asking for entries that no longer exist. And "this node has
        hit an error since it booted" does not stop being true because somebody
        pressed a button in a web page."""
        with self.node.lock:
            self.node.log_add("warn", "something to count", 4000)
            self.node.log_add("error", "something worse to count", 4001)
        before = self.get_json("/api/log")
        self.assertGreater(before["warnings"], 0)
        self.assertGreater(before["errors"], 0)

        status, body, headers = self.request("DELETE", "/api/log")
        self.assertEqual(status, 200, body)
        self.assertEqual(body, "cleared")
        self.assertTrue(headers["Content-Type"].startswith("text/plain"))

        after = self.get_json("/api/log")
        self.assertEqual(after["entries"], [])
        self.assertEqual(after["total"], before["total"])
        self.assertEqual(after["warnings"], before["warnings"])
        self.assertEqual(after["errors"], before["errors"])
        # And the status document's counters agree with the log's own.
        status_log = self.get_json("/api/status")["log"]
        self.assertEqual(status_log["total"], after["total"])
        self.assertEqual(status_log["warnings"], after["warnings"])
        self.assertEqual(status_log["errors"], after["errors"])

    def test_a_cursor_survives_a_clear(self):
        """The sequence arithmetic is the reason `total` has to survive: a
        client that polls across a clear must never be handed entries it has
        already seen."""
        before = self.get_json("/api/log")
        self.request("DELETE", "/api/log")
        with self.node.lock:
            self.node.log_add("info", "after the clear", 5000)
        doc = self.get_json(f"/api/log?since={before['total']}")
        self.assertEqual([e["text"] for e in doc["entries"]], ["after the clear"])


class GnssApiTest(ApiTestCase):
    """GET /api/gnss -- what the GPS module is saying, as opposed to whether the
    node understood it. A receiver holding a fix and a receiver that is not
    wired up are the same document in /api/status, so this endpoint is the only
    thing that tells them apart."""

    FIELDS = {"present", "bytes", "ubx_frames", "nav_pvt", "nmea", "sweeps",
              "baud", "configured", "last_byte_age_ms", "last_pvt_age_ms",
              "seen", "raw_hex"}

    def test_gnss_shape(self):
        doc = self.get_json("/api/gnss")
        self.assertEqual(set(doc), self.FIELDS)
        self.assertTrue(doc["present"])
        self.assertIsInstance(doc["configured"], bool)
        for key in ("bytes", "ubx_frames", "nav_pvt", "nmea", "sweeps", "baud",
                    "last_byte_age_ms", "last_pvt_age_ms"):
            self.assertIsInstance(doc[key], int, key)
            self.assertGreaterEqual(doc[key], 0, key)
        self.assertEqual(doc["baud"], GNSS_BAUD)
        # The sweep found this module first time. A climbing count here is a
        # driver that has never held a conversation for four seconds together.
        self.assertEqual(doc["sweeps"], 0)
        # Bytes arrive whether or not anything parses, so this is non-zero from
        # the first configuration answer onwards -- and it is the field that
        # separates "no fix" from "not wired up".
        self.assertGreater(doc["bytes"], 0)
        self.assertGreaterEqual(doc["ubx_frames"], doc["nav_pvt"])

    def test_seen_is_message_types_and_counts(self):
        seen = self.get_json("/api/gnss")["seen"]
        self.assertGreater(len(seen), 0)
        # kGnssSeenTypes: the table is small on purpose.
        self.assertLessEqual(len(seen), GNSS_SEEN_TYPES)
        for entry in seen:
            self.assertEqual(set(entry), {"msg", "count"})
            # Class and id as two lower-case hex bytes: "01:07" is NAV-PVT.
            self.assertRegex(entry["msg"], GNSS_MSG_RE)
            self.assertIsInstance(entry["count"], int)
            # A slot the driver never filled is omitted, not sent at zero.
            self.assertGreater(entry["count"], 0)

    def test_raw_hex_is_lower_case_hex_of_even_length(self):
        raw_hex = self.get_json("/api/gnss")["raw_hex"]
        self.assertIsInstance(raw_hex, str)
        self.assertRegex(raw_hex, LOWER_HEX_RE)
        self.assertEqual(len(raw_hex) % 2, 0)
        # Up to the last 192 bytes off the wire (kSniffBytes), and no more.
        self.assertLessEqual(len(raw_hex), GNSS_SNIFF_BYTES * 2)
        self.assertGreater(len(raw_hex), 0)

    def test_raw_hex_decodes_to_a_checksum_valid_ubx_frame(self):
        """The raw tap is the field that settles arguments, and it is only worth
        anything if what comes out of it really parses. The window is a ring, so
        it starts part-way through a frame and a reader has to resynchronise on
        the sync pair -- which is exactly what the firmware's parser does."""
        raw = bytes.fromhex(self.get_json("/api/gnss")["raw_hex"])
        frames = ubx_frames_in(raw)
        self.assertGreater(len(frames), 0, "no complete frame in the raw window")
        cls, msg_id, payload = frames[0]
        self.assertEqual((cls, msg_id), (UBX_CLASS_NAV, UBX_ID_NAV_PVT))
        # NAV-PVT is 92 bytes; decodeNavPvt() rejects anything shorter.
        self.assertEqual(len(payload), 92)
        # And the fix in it agrees with the node rather than contradicting it.
        self.assertEqual(payload[20], 3)                      # fixType: 3D
        self.assertEqual(payload[21] & 0x01, 0x01)            # gnssFixOK
        self.assertEqual(payload[23], 12)                     # numSV

    def test_present_false_is_the_entire_document(self):
        """A build that takes its position from the flight controller over MSP
        has no link to report on, so it reports nothing else at all -- not even
        zeros, which would read as a receiver that is wired up and silent."""
        try:
            with self.node.lock:
                self.node.gnss_present = False
            self.assertEqual(self.get_json("/api/gnss"), {"present": False})
        finally:
            with self.node.lock:
                self.node.gnss_present = True

    def test_a_legacy_module_reports_the_older_message_set(self):
        """A NEO-6M NAKs the request for NAV-PVT and sends the legacy nav set
        instead, so nav_pvt stays 0 while the frames climb."""
        try:
            with self.node.lock:
                self.node.gnss_legacy = True
                # Far enough in that the module has sent some navigation
                # messages: at uptime 0 the only frames are the configuration
                # answers, and a message type with a zero count is omitted.
                self.node._t0 -= 10.0
            doc = self.get_json("/api/gnss")
            self.assertEqual(doc["nav_pvt"], 0)
            seen = {e["msg"]: e["count"] for e in doc["seen"]}
            for msg_id in GNSS_LEGACY_IDS:
                self.assertIn(f"{UBX_CLASS_NAV:02x}:{msg_id:02x}", seen)
            # One ACK-NAK at startup, which is expected on such a module and is
            # not an error: it is the only thing that distinguishes it from a
            # receiver that was never plugged in.
            self.assertEqual(seen[f"{UBX_CLASS_ACK:02x}:{UBX_ID_ACK_NAK:02x}"], 1)
            self.assertNotIn(f"{UBX_CLASS_NAV:02x}:{UBX_ID_NAV_PVT:02x}", seen)
        finally:
            with self.node.lock:
                self.node.gnss_legacy = False
                self.node._t0 += 10.0


class ConfigApiTest(ApiTestCase):
    def setUp(self):
        self.request("POST", "/api/config/reset")
        # /api/config/save is rate limited on the firmware (flash wears out);
        # clear that history so tests are independent of each other's timing.
        with self.node.lock:
            self.node.ever_saved = False

    def test_get_config_matches_the_defaults(self):
        self.assertEqual(self.get_json("/api/config"), config_to_json(default_config()))

    def test_post_merges_and_returns_the_new_config(self):
        status, body, _ = self.request(
            "POST", "/api/config",
            {"follow": {"ofsLongM": -20}, "rate": {"target_load": 0.2}})
        self.assertEqual(status, 200, body)
        doc = json.loads(body)
        self.assertEqual(doc["follow"]["ofsLongM"], -20.0)
        self.assertEqual(doc["rate"]["target_load"], 0.2)
        self.assertEqual(doc["rate"]["jitter_frac"], 0.25)
        # The GET agrees with what the POST returned.
        self.assertEqual(self.get_json("/api/config"), doc)

    def test_rejected_post_is_400_with_the_message_and_changes_nothing(self):
        before = self.get_json("/api/config")
        status, body, headers = self.request(
            "POST", "/api/config", {"node": {"name": "NOPE"}, "gnss": {"rate_hz": 99}})
        self.assertEqual(status, 400)
        self.assertTrue(headers["Content-Type"].startswith("text/plain"))
        self.assertEqual(body, "gnss.rate_hz must be 1-25")
        self.assertEqual(self.get_json("/api/config"), before)

    def test_secrets_are_redacted_on_get_and_round_trip_on_post(self):
        status, body, _ = self.request(
            "POST", "/api/config", {"security": {"passphrase": "groupkey"}})
        self.assertEqual(status, 200, body)
        doc = json.loads(body)
        self.assertEqual(doc["security"]["passphrase"], REDACTED_SECRET)
        with self.node.lock:
            self.assertEqual(self.node.config["security"]["passphrase"], "groupkey")
        # Post the whole document straight back, placeholder and all.
        status, body, _ = self.request("POST", "/api/config", doc)
        self.assertEqual(status, 200, body)
        with self.node.lock:
            self.assertEqual(self.node.config["security"]["passphrase"], "groupkey")

    def test_bad_json_is_400(self):
        status, body, _ = self.request("POST", "/api/config", b"{not json")
        self.assertEqual(status, 400)
        self.assertIn("JSON", body)

    def test_save_then_reset(self):
        status, _, _ = self.request("POST", "/api/config", {"node": {"name": "SAVED"}})
        self.assertEqual(status, 200)
        status, body, _ = self.request("POST", "/api/config/save")
        self.assertEqual(status, 200, body)
        self.assertEqual(body, "saved")
        with self.node.lock:
            self.assertEqual(self.node.saved_config["node"]["name"], "SAVED")
        status, body, _ = self.request("POST", "/api/config/reset")
        self.assertEqual(status, 200, body)
        self.assertEqual(body, "reset")
        self.assertEqual(self.get_json("/api/config"), config_to_json(default_config()))

    def test_save_is_rate_limited(self):
        """Flash has a finite number of erase cycles; a Save button does not."""
        self.assertEqual(self.request("POST", "/api/config/save")[0], 200)
        status, body, _ = self.request("POST", "/api/config/save")
        self.assertEqual(status, 429)
        self.assertIn("recently", body)

    def test_reset_sets_reboot_required(self):
        """Radio and WiFi settings only take effect at boot, and a factory reset
        almost certainly changed some."""
        self.assertEqual(self.request("POST", "/api/config/reset")[0], 200)
        self.assertTrue(self.get_json("/api/status")["reboot_required"])

    def test_post_is_ram_only_until_save(self):
        """A setting that makes the node unreachable is recoverable with a power
        cycle, which is the whole reason POST does not persist."""
        self.request("POST", "/api/config/save")
        status, _, _ = self.request("POST", "/api/config", {"node": {"name": "TEMP"}})
        self.assertEqual(status, 200)
        self.node.reboot()
        self.assertEqual(self.get_json("/api/config")["node"]["name"], "")


class SimApiTest(ApiTestCase):
    def setUp(self):
        self.request("POST", "/api/config/reset")
        status, body, _ = self.request("POST", "/api/config", {"sim": {"enabled": True}})
        self.assertEqual(status, 200, body)
        self.request("POST", "/api/sim/clear")

    def test_sim_is_readable_and_reports_enabled(self):
        doc = self.get_json("/api/sim")
        self.assertEqual(set(doc), {"enabled", "peers"})
        self.assertTrue(doc["enabled"])
        self.assertEqual(doc["peers"], [])

    def test_add_peer_then_list_then_delete(self):
        status, body, _ = self.request("POST", "/api/sim/peer", {
            "uid": "5eed0001", "name": "SIM1", "mode": "hex", "lat": 37.0,
            "lon": -122.0, "alt_m": 120, "speed_ms": 15, "course_deg": 90,
            "radius_m": 150})
        self.assertEqual(status, 200, body)
        self.assertEqual(body, "ok")

        listed = self.get_json("/api/sim")["peers"]
        self.assertEqual(len(listed), 1)
        peer = listed[0]
        self.assertEqual(set(peer), {"uid", "name", "mode", "lat", "lon", "alt_m",
                                     "speed_ms", "course_deg", "radius_m",
                                     "elapsed_ms", "running"})
        self.assertEqual(peer["uid"], "5eed0001")
        self.assertEqual(peer["name"], "SIM1")
        self.assertEqual(peer["mode"], "hex")
        # Decimal degrees here, the same units the POST body takes.
        self.assertAlmostEqual(peer["lat"], 37.0)
        self.assertAlmostEqual(peer["lon"], -122.0)
        self.assertTrue(peer["running"])
        self.assertGreaterEqual(peer["elapsed_ms"], 0)

        status, body, _ = self.request("DELETE", "/api/sim/peer?uid=5eed0001")
        self.assertEqual(status, 200, body)
        self.assertEqual(body, "removed")
        self.assertEqual(self.get_json("/api/sim")["peers"], [])

    def test_uid_is_generated_when_absent(self):
        status, body, _ = self.request("POST", "/api/sim/peer",
                                       {"name": "AUTO", "mode": "circle"})
        self.assertEqual(status, 200, body)
        peers = self.get_json("/api/sim")["peers"]
        self.assertEqual(len(peers), 1)
        self.assertRegex(peers[0]["uid"], UID_RE)
        # Generated UIDs are marked so a simulated peer is recognisable as one
        # even in a raw packet capture.
        self.assertTrue(peers[0]["uid"].startswith("5eed"))

    def test_a_second_post_replaces_rather_than_duplicates(self):
        """A sim POST is a replace, not a merge: the firmware builds a fresh
        SimPeerConfig, so a key the body omits takes the struct default."""
        self.request("POST", "/api/sim/peer",
                     {"uid": "5eed0002", "mode": "line", "name": "FIRST",
                      "speed_ms": 30, "radius_m": 500})
        status, body, _ = self.request("POST", "/api/sim/peer",
                                       {"uid": "5eed0002", "mode": "circle",
                                        "radius_m": 200})
        self.assertEqual(status, 200, body)
        peers = self.get_json("/api/sim")["peers"]
        self.assertEqual(len(peers), 1)
        self.assertEqual(peers[0]["mode"], "circle")
        self.assertEqual(peers[0]["radius_m"], 200)
        self.assertEqual(peers[0]["name"], "SIM")    # the default, not "FIRST"
        self.assertEqual(peers[0]["speed_ms"], 0.0)  # the default, not 30

    def test_bad_field_values_are_400(self):
        status, body, _ = self.request("POST", "/api/sim/peer", {"mode": "spiral"})
        self.assertEqual(status, 400)
        self.assertEqual(body, "mode must be static, line, circle or hex")
        status, body, _ = self.request("POST", "/api/sim/peer",
                                       {"mode": "line", "speed_ms": 500})
        self.assertEqual(status, 400)
        self.assertEqual(body, "speed_ms must be 0-200")
        status, body, _ = self.request("POST", "/api/sim/peer",
                                       {"mode": "circle", "radius_m": 0})
        self.assertEqual(status, 400)
        self.assertEqual(body, "radius_m must be > 0 for circle and hex")

    def test_the_peer_table_has_a_ceiling(self):
        for i in range(4):
            status, body, _ = self.request("POST", "/api/sim/peer",
                                           {"uid": "5eed001%d" % i, "mode": "static"})
            self.assertEqual(status, 200, body)
        status, body, _ = self.request("POST", "/api/sim/peer",
                                       {"uid": "5eed00ff", "mode": "static"})
        self.assertEqual(status, 409)
        self.assertEqual(body, "simulated peer table full")

    def test_delete_needs_a_uid_and_404s_on_an_unknown_one(self):
        status, body, _ = self.request("DELETE", "/api/sim/peer")
        self.assertEqual(status, 400)
        self.assertEqual(body, "uid required")
        status, body, _ = self.request("DELETE", "/api/sim/peer?uid=deadbeef")
        self.assertEqual(status, 404)
        self.assertEqual(body, "no such peer")

    def test_clear_removes_all_of_them(self):
        self.request("POST", "/api/sim/peer", {"uid": "5eed0003", "mode": "hex"})
        self.request("POST", "/api/sim/peer", {"uid": "5eed0004", "mode": "line"})
        self.assertEqual(len(self.get_json("/api/sim")["peers"]), 2)
        status, body, _ = self.request("POST", "/api/sim/clear")
        self.assertEqual(status, 200, body)
        self.assertEqual(body, "cleared")
        self.assertEqual(self.get_json("/api/sim")["peers"], [])

    def test_mutations_are_refused_while_sim_is_disabled(self):
        self.request("POST", "/api/sim/clear")
        status, body, _ = self.request("POST", "/api/config", {"sim": {"enabled": False}})
        self.assertEqual(status, 200, body)
        status, body, _ = self.request("POST", "/api/sim/peer", {"mode": "hex"})
        self.assertEqual(status, 409)
        self.assertEqual(body, "set sim.enabled before adding peers")
        # Reading still works, so a UI can render the panel and say why.
        self.assertFalse(self.get_json("/api/sim")["enabled"])

    def test_a_simulated_peer_shows_up_in_status(self):
        self.request("POST", "/api/sim/peer",
                     {"uid": "5eed0005", "name": "SIMX", "mode": "circle",
                      "lat": 37.0, "lon": -122.0, "speed_ms": 20, "radius_m": 120})
        doc = self.get_json("/api/status")
        self.assertIn("5eed0005", [p["uid"] for p in doc["peers"]])
        self.assertEqual(doc["sim"]["peers"], 1)
        # The virtual radio is flagged so the UI can say the numbers are not RF.
        sim_radios = [r for r in doc["radios"] if r["sim"]]
        self.assertEqual(len(sim_radios), 1)


class SystemApiTest(ApiTestCase):
    def test_reboot_answers_before_it_reboots(self):
        status, body, _ = self.request("POST", "/api/system/reboot")
        self.assertEqual(status, 200)
        self.assertTrue(body)

    @staticmethod
    def firmware_upload(filename):
        return (b"------x\r\nContent-Disposition: form-data; name=\"firmware\"; "
                b"filename=\"" + filename.encode() + b"\"\r\n"
                b"Content-Type: application/octet-stream\r\n\r\n"
                + b"\xe9" * 512 + b"\r\n------x--\r\n")

    def test_update_accepts_a_bin(self):
        status, body, headers = self.request(
            "POST", "/update", self.firmware_upload("fw.bin"),
            ctype="multipart/form-data; boundary=----x")
        self.assertEqual(status, 200, body)
        self.assertTrue(headers["Content-Type"].startswith("text/plain"))
        self.assertEqual(body, "update complete, rebooting")

    def test_update_refuses_anything_but_a_firmware_image(self):
        status, body, _ = self.request(
            "POST", "/update", self.firmware_upload("holiday.jpg"),
            ctype="multipart/form-data; boundary=----x")
        self.assertEqual(status, 400)
        self.assertEqual(body, "must upload .bin or .bin.gz")

    def test_unknown_api_path_is_404_text(self):
        status, body, headers = self.request("GET", "/api/nope")
        self.assertEqual(status, 404)
        self.assertTrue(headers["Content-Type"].startswith("text/plain"))

    def test_static_files_are_served_and_the_endpoint_prefix_is_rewritten(self):
        status, body, headers = self.request("GET", "/main.js")
        self.assertEqual(status, 200)
        self.assertIn("javascript", headers["Content-Type"])
        self.assertNotIn("192.168.4.1", body.split("\n")[0])
        self.assertIn('const ENDPOINT_PREFIX = ""', body)

    def test_directory_traversal_is_refused(self):
        status, _, _ = self.request("GET", "/../platformio.ini")
        self.assertEqual(status, 404)


# ---------------------------------------------------------------------------
# The live model, driven directly so the clock can be moved
# ---------------------------------------------------------------------------

class LiveModelTest(unittest.TestCase):
    def aged_node(self, seconds=12.0, config_patch=None):
        node = MockNode()
        if config_patch:
            merged, err = merge_config(config_patch, node.config)
            self.assertIsNone(err, err)
            node.config = merged
        node._t0 -= seconds
        node.advance()
        return node

    @staticmethod
    def lora(node):
        return {r["name"]: r
                for r in node.status_json(node.uptime_ms())["radios"]}["LORA"]

    def test_follow_walks_idle_then_acquiring_then_locked(self):
        node = MockNode()
        node.advance()
        self.assertEqual(node.follow_json(node.uptime_ms())["state"], "IDLE")

        node._t0 -= 2.0   # past the gate, not yet locked
        node.advance()
        self.assertEqual(node.follow_json(node.uptime_ms())["state"], "ACQUIRING")

        node._t0 -= 10.0
        node.advance()
        follow = node.follow_json(node.uptime_ms())
        self.assertEqual(follow["state"], "LOCKED")
        self.assertTrue(follow["gate_active"])
        self.assertRegex(follow["locked_uid"], UID_RE)
        self.assertTrue(follow["locked_name"])

    def test_locked_follow_publishes_a_target_and_a_live_offset(self):
        node = self.aged_node()
        follow = node.follow_json(node.uptime_ms())
        self.assertEqual(follow["state"], "LOCKED")
        self.assertEqual(set(follow["target"]),
                         {"lat", "lon", "alt_cm", "heading_deg", "age_ms"})
        self.assertIsInstance(follow["target"]["lat"], int)
        self.assertIsInstance(follow["target"]["alt_cm"], int)
        self.assertTrue(1 <= follow["target"]["heading_deg"] <= 360)
        self.assertEqual(set(follow["live_offset"]), {"long_m", "lat_m", "vert_m"})
        self.assertEqual(follow["live_offset"]["long_m"], -15.0)
        # These two are only meaningful alongside a target, so the firmware only
        # emits them when it has one.
        self.assertIn("autothrottle_engaged", follow)
        self.assertIn("target_speed_cms", follow)

    def test_gvar_values_appear_only_once_their_slot_is_enabled(self):
        node = self.aged_node(config_patch={
            "follow": {"statusGvarIndex": 6, "conditionFlagsGvarIndex": 7}})
        follow = node.follow_json(node.uptime_ms())
        self.assertEqual(follow["status_gvar"], 2)   # FOLLOW_LOCK_LOCKED
        self.assertEqual(follow["condition_gvar"], 0)

    def test_prearm_offset_appears_once_an_rc_axis_is_assigned(self):
        node = self.aged_node(config_patch={"follow": {"rcLongChannel": 5}})
        follow = node.follow_json(node.uptime_ms())
        self.assertEqual(set(follow["prearm_offset"]), {"long_m", "lat_m", "vert_m"})

    def test_counters_climb_and_the_frame_ring_rolls(self):
        node = self.aged_node()
        doc = node.status_json(node.uptime_ms())
        self.assertGreater(doc["stats"]["beacons_sent"], 0)
        self.assertGreater(doc["stats"]["rx_ok"], 0)
        self.assertGreater(sum(r["tx"] for r in doc["radios"]), 0)
        frames = node.frames_json()
        # The ring is full and `total` has run past its capacity, so a client
        # polling with `since` can tell it missed some.
        self.assertEqual(len(frames["frames"]), FRAME_LOG_CAPACITY)
        self.assertGreater(frames["total"], frames["capacity"])

    def test_the_frame_log_carries_errors_for_the_debug_view(self):
        node = self.aged_node(seconds=60.0)
        seen = {f["result"] for f in node.frames.__iter__()}
        # Everything the log has produced is a documented result value.
        self.assertTrue(seen.issubset(set(FRAME_RESULTS)), seen)
        # Over a minute of traffic the error paths must have been exercised, or
        # the debug view's error styling is never visible in the mock.
        history = []
        for _ in range(3):
            history.extend(f["result"] for f in node.frames)
            node._t0 -= 30.0
            node.advance()
        self.assertIn("crypto_fail", history)
        self.assertIn("tx", history)
        self.assertIn("ok", history)

    def test_peers_move_and_age(self):
        node = self.aged_node()
        first = node.status_json(node.uptime_ms())["peers"]
        node._t0 -= 3.0
        node.advance()
        second = node.status_json(node.uptime_ms())["peers"]
        moved = [a["lat"] != b["lat"] or a["lon"] != b["lon"]
                 for a, b in zip(first, second)]
        self.assertTrue(all(moved), "every peer should have moved")
        for peer in second:
            self.assertGreater(peer["packets"], 0)
            self.assertLess(peer["age_ms"], 5000)

    def test_a_simulated_peer_enters_the_frame_log(self):
        node = MockNode()
        merged, err = merge_config({"sim": {"enabled": True}}, node.config)
        self.assertIsNone(err, err)
        node.config = merged
        peer, perr, _code = node.upsert_sim_peer(
            {"uid": "5eed0009", "name": "SIMLOG", "mode": "line", "lat": 37.0,
             "lon": -122.0, "speed_ms": 18, "course_deg": 45}, 0)
        self.assertIsNone(perr, perr)
        node._t0 -= 12.0
        node.advance()
        self.assertIn("5eed0009", [f["uid"] for f in node.frames])
        self.assertIn("5eed0009", [p["uid"] for p in node.status_json(node.uptime_ms())["peers"]])
        self.assertGreater(peer.packets, 0)

    def test_every_sim_mode_produces_a_moving_position(self):
        node = MockNode()
        node.config["sim"]["enabled"] = True
        for mode in ("static", "line", "circle", "hex"):
            with self.subTest(mode=mode):
                node.clear_sim_peers()
                peer, err, _code = node.upsert_sim_peer(
                    {"name": mode.upper()[:6], "mode": mode, "lat": 37.0, "lon": -122.0,
                     "speed_ms": 20, "course_deg": 30, "radius_m": 100}, 0)
                self.assertIsNone(err, err)
                a = peer.position(0.0)
                b = peer.position(7.0)
                if mode == "static":
                    self.assertEqual(a[:2], b[:2])
                    self.assertEqual(b[3], 0)
                else:
                    self.assertNotEqual(a[:2], b[:2])
                    self.assertGreater(b[3], 0)
                self.assertTrue(0 <= b[4] <= 3599)

    def test_the_hex_path_closes_and_climbs(self):
        node = MockNode()
        node.config["sim"]["enabled"] = True
        peer, err, _code = node.upsert_sim_peer(
            {"name": "HEX", "mode": "hex", "lat": 37.0, "lon": -122.0,
             "speed_ms": 60, "radius_m": 120, "alt_m": 100}, 0)
        self.assertIsNone(err, err)
        side, speed = peer.radius_m, peer.speed_ms
        lap_s = side * 6 / speed
        start = peer.position(0.0)
        close = peer.position(lap_s)
        self.assertAlmostEqual(start[0], close[0], places=6)
        self.assertAlmostEqual(start[1], close[1], places=6)
        # Peak altitude at the half-way vertex, back to the base at the close.
        # ff::kSimHexClimbM is 80 m, and SimPeerState::alt_m is whole metres.
        self.assertEqual(start[2], 100)
        self.assertEqual(peer.position(lap_s / 2)[2], 180)
        self.assertEqual(close[2], 100)

    def test_reboot_restores_the_saved_config_and_restarts_the_clock(self):
        node = self.aged_node()
        self.assertGreater(node.frame_total, 0)
        merged, err = merge_config({"node": {"name": "TEMP"}}, node.config)
        self.assertIsNone(err, err)
        node.config = merged
        node.reboot()
        self.assertEqual(node.config["node"]["name"], "")
        self.assertEqual(node.frame_total, 0)
        self.assertLess(node.uptime_ms(), 1000)

    def test_beacon_interval_follows_the_aloha_formula(self):
        """(N_total * airtime) / G_target, clamped -- lib/ff_core/rate_control.h."""
        node = self.aged_node()
        for radio in node.status_json(node.uptime_ms())["radios"]:
            self.assertGreaterEqual(radio["beacon_interval_ms"],
                                    node.config["rate"]["min_interval_ms"])
            self.assertLessEqual(radio["beacon_interval_ms"],
                                 node.config["rate"]["max_interval_ms"])

    def test_crypto_mode_follows_the_passphrase(self):
        node = self.aged_node()
        self.assertEqual(node.status_json(node.uptime_ms())["crypto"]["mode"], "ccm")
        merged, err = merge_config({"security": {"passphrase": "none"}}, node.config)
        self.assertIsNone(err, err)
        node.config = merged
        self.assertEqual(node.status_json(node.uptime_ms())["crypto"]["mode"], "none")

    def test_node_name_is_derived_from_the_uid_when_unset(self):
        node = MockNode()
        derived = node.node_name()
        self.assertTrue(derived)
        self.assertLessEqual(len(derived), MAX_NAME_LEN)
        merged, err = merge_config({"node": {"name": "BRAVO"}}, node.config)
        self.assertIsNone(err, err)
        node.config = merged
        self.assertEqual(node.node_name(), "BRAVO")

    def test_locked_uid_is_always_present_and_zero_when_idle(self):
        """FollowStatus is serialised whole, so locked_uid/locked_name are always
        there; an unlocked node reports the zero UID and an empty name rather
        than omitting the keys."""
        node = MockNode()
        node.advance()
        follow = node.follow_json(node.uptime_ms())
        self.assertEqual(follow["state"], "IDLE")
        self.assertEqual(follow["locked_uid"], "00000000")
        self.assertEqual(follow["locked_name"], "")
        self.assertNotIn("target", follow)
        self.assertNotIn("live_offset", follow)
        self.assertNotIn("autothrottle_engaged", follow)

    def test_crypto_counters_vanish_with_the_cipher(self):
        """No cipher, no cipher counters -- the firmware emits `mode` alone."""
        node = self.aged_node()
        merged, err = merge_config({"security": {"passphrase": "none"}}, node.config)
        self.assertIsNone(err, err)
        node.config = merged
        self.assertEqual(node.status_json(node.uptime_ms())["crypto"], {"mode": "none"})

    def test_a_sim_peers_path_starts_when_it_is_created(self):
        node = self.aged_node(config_patch={"sim": {"enabled": True}})
        now = node.uptime_ms()
        peer, err, _code = node.upsert_sim_peer(
            {"uid": "5eed00aa", "mode": "line", "lat": 37.0, "lon": -122.0,
             "speed_ms": 20, "course_deg": 90}, now)
        self.assertIsNone(err, err)
        # Elapsed time runs from the moment of the call, not from node boot, so
        # a peer added after an hour of uptime still starts at its origin.
        self.assertEqual(peer.elapsed_ms(now), 0)
        lat, lon, _alt, _spd, _crs = peer.state(now)
        self.assertAlmostEqual(lat, 37.0, places=9)
        self.assertAlmostEqual(lon, -122.0, places=9)

    def test_the_log_trickles_and_exercises_every_level(self):
        """The log view needs something to show, and its per-level styling needs
        all four levels to appear -- warnings often enough to be visible within
        a minute, errors rarely enough to still read as an event."""
        node = self.aged_node(seconds=45.0)
        first = node.log_json()
        self.assertGreater(len(first["entries"]), 3)
        self.assertTrue(all(e["level"] in LOG_LEVELS for e in first["entries"]))

        seen = set()
        total = first["total"]
        for _ in range(6):
            doc = node.log_json()
            seen.update(e["level"] for e in doc["entries"])
            self.assertGreaterEqual(doc["total"], total)   # never backwards
            total = doc["total"]
            node._t0 -= 45.0
            node.advance()
        self.assertEqual(seen, set(LOG_LEVELS), seen)
        self.assertGreater(node.log_warnings, 0)
        self.assertGreater(node.log_errors, 0)

    def test_the_log_ring_rolls_and_total_keeps_climbing(self):
        node = self.aged_node(seconds=300.0)
        doc = node.log_json()
        self.assertEqual(len(doc["entries"]), LOG_CAPACITY)
        self.assertGreater(doc["total"], doc["capacity"])
        # A clear empties the ring and touches nothing else.
        node.clear_log()
        after = node.log_json()
        self.assertEqual(after["entries"], [])
        self.assertEqual(after["total"], doc["total"])
        self.assertEqual(after["warnings"], doc["warnings"])
        self.assertEqual(after["errors"], doc["errors"])

    def test_loop_stats_move_and_overruns_stay_rare(self):
        """A loop of a few hundred microseconds with the occasional long one.
        The mean hides the spikes completely, which is why max and overruns are
        published at all."""
        node = self.aged_node(seconds=90.0)
        first = node.loop_json()
        self.assertGreater(first["samples"], 0)
        self.assertTrue(100 <= first["mean_us"] <= 2000, first["mean_us"])
        self.assertLessEqual(first["min_us"], first["mean_us"])
        self.assertLessEqual(first["mean_us"], first["max_us"])
        self.assertEqual(first["rate_hz"], 1000000 // first["mean_us"])
        # Long enough to have collected an overrun, and the spike that caused it
        # has to be above the threshold or the counter is lying.
        self.assertGreater(first["overruns"], 0)
        self.assertGreater(first["max_us"], first["overrun_threshold_us"])
        # Rare: an overrun on every other loop would be a different bug report.
        self.assertLess(first["overruns"], first["samples"] / 1000.0)

        node._t0 -= 10.0
        node.advance()
        second = node.loop_json()
        self.assertGreater(second["samples"], first["samples"])
        self.assertGreaterEqual(second["max_us"], first["max_us"])
        self.assertGreaterEqual(second["overruns"], first["overruns"])

    def test_the_overrun_threshold_is_the_shortest_beacon_interval(self):
        """main.cpp snapshots it from rate.min_interval_ms at boot: a loop
        longer than that can miss a transmission outright."""
        node = self.aged_node(config_patch={"rate": {"min_interval_ms": 250}})
        self.assertEqual(node.loop_json()["overrun_threshold_us"], 100 * 1000)
        node.saved_config = copy.deepcopy(node.config)
        node.reboot()
        node.advance()
        self.assertEqual(node.loop_json()["overrun_threshold_us"], 250 * 1000)

    def test_a_reboot_shows_up_as_a_software_restart(self):
        """Why the node last restarted is the single most useful thing to know
        about a board that has been misbehaving."""
        node = self.aged_node()
        self.assertEqual(node.system_json(node.uptime_ms())["reset_reason"],
                         RESET_REASON_POWERON)
        node.reboot()
        node.advance()
        self.assertEqual(node.system_json(node.uptime_ms())["reset_reason"],
                         RESET_REASON_SOFTWARE)
        # A power cycle does clear the log, counters included -- unlike a clear.
        self.assertLess(node.log_total, 10)

    def test_min_free_heap_is_a_low_water_mark(self):
        node = self.aged_node(seconds=60.0)
        doc = node.status_json(node.uptime_ms())
        self.assertLessEqual(doc["system"]["min_free_heap"], doc["system"]["free_heap"])
        floor = doc["system"]["min_free_heap"]
        for _ in range(5):
            node._t0 -= 7.0
            node.advance()
            now = node.status_json(node.uptime_ms())["system"]["min_free_heap"]
            self.assertLessEqual(now, floor)   # only ever downwards
            floor = now

    def test_lora_reports_snr_once_it_has_received_something(self):
        """has_snr goes true on the first frame the driver receives, so a LoRa
        radio that has not heard anything yet omits the field."""
        node = self.aged_node(seconds=30.0)
        radios = {r["name"]: r for r in node.status_json(node.uptime_ms())["radios"]}
        self.assertIsInstance(radios["LORA"]["last_snr_db"], float)
        self.assertNotIn("last_snr_db", radios["ESPNOW"])
        self.assertNotIn("modulation", radios["ESPNOW"])

    def test_lora_defers_rather_than_drops(self):
        """The beacon and the announce are independent schedules on one
        half-duplex radio and collide several times a minute by design. The
        driver holds the second frame an airtime and sends it, so a healthy node
        climbs tx_deferred and leaves tx_dropped alone -- the old behaviour was
        a drop count climbing on a radio that was working perfectly."""
        node = self.aged_node(seconds=30.0)
        lora = self.lora(node)
        # Roughly one collision every six seconds, so half a minute has some.
        self.assertGreater(lora["tx_deferred"], 0)
        self.assertEqual(lora["tx_dropped"], 0)
        self.assertEqual(lora["tx_timeouts"], 0)

        node._t0 -= 60.0
        node.advance()
        later = self.lora(node)
        self.assertGreater(later["tx_deferred"], lora["tx_deferred"])
        # A deferral is not a loss and a timeout is not a thing a healthy radio
        # does: neither of the two fault counters moves with them.
        self.assertEqual(later["tx_dropped"], 0)
        self.assertEqual(later["tx_timeouts"], 0)
        # Paced by node time rather than by this mock's tick, so the rate is the
        # one the two schedules actually produce. Generous bounds: what matters
        # is the order of magnitude, not the exact arithmetic.
        gained = later["tx_deferred"] - lora["tx_deferred"]
        expected = 60000 // LORA_TX_DEFER_EVERY_MS
        self.assertGreaterEqual(gained, expected - 1)
        self.assertLessEqual(gained, expected + 1)

    def test_transmit_counters_never_go_backwards(self):
        """They are since-boot totals. A UI that diffs two polls to get a rate
        reads a negative one if they ever step back."""
        node = self.aged_node(seconds=5.0)
        seen = {}
        for _ in range(6):
            for radio in node.status_json(node.uptime_ms())["radios"]:
                for key in ("tx", "tx_deferred", "tx_dropped", "tx_timeouts"):
                    prev = seen.get((radio["name"], key), 0)
                    self.assertGreaterEqual(radio[key], prev,
                                            f"{radio['name']}.{key}")
                    seen[(radio["name"], key)] = radio[key]
            node._t0 -= 7.0
            node.advance()
        # And the run actually went somewhere, or the check above proves nothing.
        self.assertGreater(seen[("LORA", "tx_deferred")], 0)

    def test_the_simulated_radio_has_no_modulation_and_does_not_transmit(self):
        node = self.aged_node(config_patch={"sim": {"enabled": True}})
        radios = {r["name"]: r for r in node.status_json(node.uptime_ms())["radios"]}
        self.assertIn("SIM", radios)
        # It injects frames through the real receive path and never keys an
        # antenna, so it has neither a frequency to describe nor an SNR.
        self.assertNotIn("modulation", radios["SIM"])
        self.assertNotIn("last_snr_db", radios["SIM"])
        self.assertFalse(radios["SIM"]["transmits"])
        self.assertTrue(radios["LORA"]["transmits"])
        # Nothing to defer and no interrupt to miss on a radio that never keys
        # an antenna, but the fields are still there at zero.
        self.assertEqual(radios["SIM"]["tx_deferred"], 0)
        self.assertEqual(radios["SIM"]["tx_timeouts"], 0)

    def test_gnss_counters_climb_with_node_time(self):
        """Paced by the module's 5 Hz solution rather than by how often anything
        polls, so the numbers have the shape the hardware produces."""
        node = self.aged_node(seconds=10.0)
        first = node.gnss_json(node.uptime_ms())
        self.assertGreater(first["nav_pvt"], 0)

        node._t0 -= 30.0
        node.advance()
        later = node.gnss_json(node.uptime_ms())
        for key in ("bytes", "ubx_frames", "nav_pvt"):
            self.assertGreater(later[key], first[key], key)
        # One NAV-PVT per navigation epoch. Generous bounds: what matters is the
        # order of magnitude, not the exact arithmetic.
        gained = later["nav_pvt"] - first["nav_pvt"]
        expected = 30000 // GNSS_NAV_RATE_MS
        self.assertGreaterEqual(gained, expected - 1)
        self.assertLessEqual(gained, expected + 1)
        # The NMEA that arrived before the port configuration took effect, and
        # nothing since: a climbing count here would mean it never took effect.
        self.assertEqual(later["nmea"], first["nmea"])
        self.assertEqual(later["sweeps"], 0)

    def test_gnss_counters_never_go_backwards(self):
        """Since-boot totals. A UI that diffs two polls to get a rate reads a
        negative one if they ever step back."""
        node = self.aged_node(seconds=5.0)
        seen = {}
        for _ in range(6):
            doc = node.gnss_json(node.uptime_ms())
            for key in ("bytes", "ubx_frames", "nav_pvt", "nmea", "sweeps"):
                self.assertGreaterEqual(doc[key], seen.get(key, 0), key)
                seen[key] = doc[key]
            node._t0 -= 7.0
            node.advance()
        # And the run actually went somewhere, or the check above proves nothing.
        self.assertGreater(seen["nav_pvt"], 0)

    def test_a_legacy_module_climbs_frames_but_never_nav_pvt(self):
        """UBX-NAV-PVT only exists from u-blox protocol 14. An older module -- a
        NEO-6M, as fitted to many T-Beams -- NAKs the request for it, and the
        driver falls back to the legacy nav set. nav_pvt then stays 0 forever
        while ubx_frames climbs, and that is healthy rather than broken."""
        node = MockNode(gnss_legacy=True)
        node._t0 -= 10.0
        node.advance()
        first = node.gnss_json(node.uptime_ms())

        node._t0 -= 30.0
        node.advance()
        later = node.gnss_json(node.uptime_ms())

        self.assertEqual(first["nav_pvt"], 0)
        self.assertEqual(later["nav_pvt"], 0)
        self.assertGreater(later["ubx_frames"], first["ubx_frames"])
        self.assertGreater(later["bytes"], first["bytes"])
        # Four messages carrying between them what the one modern message
        # carries on its own, all climbing together.
        before = {e["msg"]: e["count"] for e in first["seen"]}
        after = {e["msg"]: e["count"] for e in later["seen"]}
        for msg_id in GNSS_LEGACY_IDS:
            key = f"{UBX_CLASS_NAV:02x}:{msg_id:02x}"
            self.assertIn(key, after)
            self.assertGreater(after[key], before[key], key)
        # No NAV-PVT ever arrives, so its age reads 0 -- "never happened", not
        # "just happened". Reading it the other way round is exactly backwards,
        # which is why bytes and ubx_frames are the fields to check.
        self.assertEqual(later["last_pvt_age_ms"], 0)

    def test_a_legacy_modules_raw_tap_is_still_real_frames(self):
        node = MockNode(gnss_legacy=True)
        node._t0 -= 10.0
        node.advance()
        raw = bytes.fromhex(node.gnss_json(node.uptime_ms())["raw_hex"])
        ids = {(cls, msg_id) for cls, msg_id, _ in ubx_frames_in(raw)}
        self.assertGreater(len(ids), 0)
        # Whatever is in there is from the legacy set, never a NAV-PVT.
        self.assertNotIn((UBX_CLASS_NAV, UBX_ID_NAV_PVT), ids)
        self.assertTrue(ids <= {(UBX_CLASS_NAV, i) for i in GNSS_LEGACY_IDS}, ids)

    def test_power_on_battery_is_self_consistent(self):
        """Nothing charges at zero volts of supply. The two cases have to stay
        internally consistent or the UI cannot be developed against either."""
        node = self.aged_node()
        node.power_usb = False
        power = node.status_json(node.uptime_ms())["power"]
        self.assertFalse(power["usb_present"])
        self.assertFalse(power["charging"])
        self.assertEqual(power["supply_v"], 0.0)
        self.assertEqual(power["charge_ma"], 0.0)
        self.assertGreater(power["discharge_ma"], 0.0)
        self.assertGreater(power["battery_v"], 3.0)
        self.assertTrue(0 <= power["battery_pct"] <= 100)

    def test_power_with_no_battery_attached(self):
        node = self.aged_node()
        node.power_battery_present = False
        power = node.status_json(node.uptime_ms())["power"]
        self.assertFalse(power["battery_present"])
        self.assertFalse(power["charging"])
        self.assertEqual(power["battery_v"], 0.0)
        self.assertEqual(power["charge_ma"], 0.0)
        self.assertEqual(power["discharge_ma"], 0.0)
        # No battery, no estimate to make.
        self.assertNotIn("battery_pct", power)

    def test_listen_only_and_sim_flags_reach_the_status_document(self):
        node = self.aged_node(config_patch={"node": {"listen_only": True},
                                            "sim": {"enabled": True}})
        doc = node.status_json(node.uptime_ms())
        self.assertTrue(doc["node"]["listen_only"])
        self.assertTrue(doc["sim"]["enabled"])
        self.assertIn(True, [r["sim"] for r in doc["radios"]])


if __name__ == "__main__":
    unittest.main(verbosity=2)
