#!/usr/bin/env python3
"""Runs mock_server.py's validate_config() -- a hand-maintained mirror of
FollowManager::applyConfig() (src/lib/Follow/FollowManager.cpp) -- against
the shared fixture cases in docs/spec/fixtures/follow-config-cases.json,
the same cases the C++ and JS mirrors are tested against, to keep all
three mirrors in sync.

Stdlib unittest only (no pytest in this environment, and the project has
no Python test infra to build on yet) -- run with:

    python3 test/test_mock_server.py
"""
import json
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))
from mock_server import validate_config

FIXTURE_PATH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..",
    "docs", "spec", "fixtures", "follow-config-cases.json",
)


class ValidateConfigFixtureTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(FIXTURE_PATH) as f:
            cls.fixture = json.load(f)

    def test_matches_every_fixture_case(self):
        for tc in self.fixture["cases"]:
            with self.subTest(case=tc["name"]):
                cfg = {**self.fixture["baseline"], **tc["overrides"]}
                err = validate_config(cfg)
                is_valid = err is None
                self.assertEqual(
                    is_valid, tc["expectValid"],
                    f'case "{tc["name"]}": expected expectValid={tc["expectValid"]}, '
                    f'got {is_valid} (err={err!r})',
                )


if __name__ == "__main__":
    unittest.main()
