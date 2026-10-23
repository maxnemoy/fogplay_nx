#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -Wall -Wextra -Werror -pthread -Isrc tests/async_ice_snapshot_tests.cpp -o work/async_ice_snapshot_tests
./work/async_ice_snapshot_tests | tee work/logs/async-ice-snapshot-tests.log
