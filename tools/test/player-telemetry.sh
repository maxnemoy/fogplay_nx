#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
out=$(mktemp -d /tmp/fog-player-telemetry-XXXXXX)
trap 'rm -rf "$out"' EXIT
python3 tests/generate_player_telemetry_maximum.py "$out/maximum.hpp"
python3 tests/generate_player_output_glue.py "$out/output-glue.hpp"
flags=(-std=c++17 -Wall -Wextra -Werror -pthread -g -Isrc -Itests -I"$out")
if [[ "${SANITIZE:-0}" == 1 ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
for profile in 0 1 2; do
    g++ "${flags[@]}" -DFOGPLAY_DIAGNOSTIC_PROFILE="$profile" src/core/logging/run_log.cpp src/features/streaming/application/player.cpp tests/player_telemetry_tests.cpp -Wl,--wrap=write -o "$out/test-$profile"
    timeout 40s "$out/test-$profile"
done
