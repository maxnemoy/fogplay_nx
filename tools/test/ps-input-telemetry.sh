#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
flags=(-std=c++17 -Wall -Wextra -Werror -pthread -Isrc -Ithird_party)
if [[ ${SANITIZE:-0} == 1 ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -g); fi
g++ "${flags[@]}" tests/ps_input_telemetry_tests.cpp src/core/logging/run_log.cpp -o "$out/test"
"$out/test"
