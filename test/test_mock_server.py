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

  3. The wire shape of /api/status and /api/frames against what
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
    HEADING_MODE_NAMES,
    MAX_NAME_LEN,
    MAX_PASSPHRASE_LEN,
    REDACTED_SECRET,
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
                    "follow", "sim", "wifi", "reboot_required", "config_corrupt"):
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

    def test_radio_blocks(self):
        radios = self.get_json("/api/status")["radios"]
        self.assertGreaterEqual(len(radios), 1)
        for radio in radios:
            for key in ("index", "name", "enabled", "sim", "tx", "rx_ok",
                        "rx_crypto_fail", "rx_replay", "rx_decode_fail", "rx_self",
                        "last_rssi", "last_rx_age_ms", "beacon_interval_ms",
                        "airtime_ms", "peers", "rx_dropped", "tx_dropped"):
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

    def test_listen_only_and_sim_flags_reach_the_status_document(self):
        node = self.aged_node(config_patch={"node": {"listen_only": True},
                                            "sim": {"enabled": True}})
        doc = node.status_json(node.uptime_ms())
        self.assertTrue(doc["node"]["listen_only"])
        self.assertTrue(doc["sim"]["enabled"])
        self.assertIn(True, [r["sim"] for r in doc["radios"]])


if __name__ == "__main__":
    unittest.main(verbosity=2)
