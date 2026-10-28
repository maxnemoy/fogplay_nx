#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread -Isrc \
    tests/moonlight_frame_diagnostics_tests.cpp -o work/moonlight_frame_diagnostics_tests
./work/moonlight_frame_diagnostics_tests | tee work/logs/moonlight-frame-diagnostics-tests.log
