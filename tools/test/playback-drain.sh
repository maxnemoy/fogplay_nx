#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -Wall -Wextra -Werror -Isrc tests/playback_drain_tests.cpp -o work/playback_drain_tests
./work/playback_drain_tests | tee work/logs/playback-drain-tests.log
