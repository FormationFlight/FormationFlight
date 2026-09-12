#!/usr/bin/env bash
# Runs the Follow test suite's three legs locally, mirroring the
# test-native / test-follow-logic-js / test-mock-server-py CI jobs in
# .github/workflows/build.yml. Not part of `pio run` -- PlatformIO only runs
# test envs via `pio test`, never as a side effect of building a firmware
# target, so this script is the local equivalent of running all three CI
# jobs by hand.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

echo "== native (pio test -e test_native) =="
pio test -e test_native

echo "== follow-logic.js (node --test) =="
node --test test/follow-logic.test.js

echo "== mock_server.py validate_config() =="
python3 test/test_mock_server.py
