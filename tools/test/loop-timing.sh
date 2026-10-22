#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -Wall -Wextra -Werror -Isrc tests/loop_timing_tests.cpp -o work/loop_timing_tests
./work/loop_timing_tests | tee work/logs/loop-timing-tests.log
