#!/usr/bin/env bash
# Runs every off-hardware test suite locally, mirroring .github/workflows/test.yml:
# the native Unity suites (lib/ff_core, including Follow), the web UI's
# follow-logic.js tests, and the dev mock server's config-validation tests.
# Not part of `pio run` -- PlatformIO only runs test envs via `pio test`.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

echo "== native (pio test -e native) =="
pio test -e native

echo "== follow-logic.js (node --test) =="
node --test test/follow-logic.test.js

echo "== mock_server.py validate_config() =="
python3 test/test_mock_server.py
