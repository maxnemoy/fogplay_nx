#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
output="${UI_TELEMETRY_OUTPUT:-/tmp/fogplay-ui-telemetry}"
mkdir -p "$output"
flags=(-std=c++17 -O2 -Wall -Wextra -Werror -pthread -g -Isrc)
if [[ "${SANITIZE:-0}" == 1 ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie); fi
if [[ "${TSAN:-0}" == 1 ]]; then flags+=(-fsanitize=thread -O1); fi
g++ "${flags[@]}" tests/ui_telemetry_tests.cpp src/core/logging/run_log.cpp -o "$output/ui_tests"
timeout 30s "$output/ui_tests"
for suite in loop_timing thread_observation; do
    g++ "${flags[@]}" "tests/${suite}_tests.cpp" -o "$output/$suite"
    timeout 30s "$output/$suite"
done
