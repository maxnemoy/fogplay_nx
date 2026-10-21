#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -Wall -Wextra -Werror -Isrc \
    tests/transport_reconnect_tests.cpp -o work/transport_reconnect_tests
./work/transport_reconnect_tests | tee work/logs/transport-reconnect-tests.log
python3 tests/reconnect_safety_tests.py | tee work/logs/reconnect-safety-tests.log
