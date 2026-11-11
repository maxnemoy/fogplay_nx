#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
output=$(mktemp -d "$PWD/work/logs/v2-protocol-telemetry-XXXXXX")
printf 'Protocol telemetry logs: %s\n' "$output"
for enabled in 0 1;do
 g++ -std=c++17 -Wall -Wextra -Werror -pthread -DFOGPLAY_TIMED_MUTEX_DIAGNOSTICS=$enabled -Isrc -Ithird_party tests/protocol_telemetry_tests.cpp -o "$output/protocol-$enabled" >"$output/protocol-$enabled.build.log" 2>&1
 "$output/protocol-$enabled" | tee "$output/protocol-$enabled.log"
done
g++ -std=c++17 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc -Ithird_party tests/incoming_control_diagnostics_tests.cpp -o "$output/discovery" >"$output/discovery.build.log" 2>&1
"$output/discovery" | tee "$output/discovery.log"
g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Ithird_party tests/auto_quality_tests.cpp -o "$output/auto-quality" >"$output/auto-quality.build.log" 2>&1
"$output/auto-quality" | tee "$output/auto-quality.log"
