#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
output=$(mktemp -d "$PWD/work/logs/v2-telemetry-XXXXXX")
printf 'Telemetry logs: %s\n' "$output"
flags=(-std=c++17 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer)
g++ "${flags[@]}" -Itests/stubs/app_diagnostics -Isrc src/core/logging/run_log.cpp src/app/app_diagnostics.cpp tests/telemetry_deferred_tests.cpp -o "$output/deferred" >"$output/deferred.build.log" 2>&1
"$output/deferred" | tee "$output/deferred.log"
g++ "${flags[@]}" -Isrc src/core/logging/run_log.cpp tests/run_log_tests.cpp -Wl,--wrap=write -Wl,--wrap=fsync -Wl,--wrap=rename -o "$output/run-log" >"$output/run-log.build.log" 2>&1
"$output/run-log" | tee "$output/run-log.log"
g++ "${flags[@]}" -Itests/stubs/app_diagnostics -Isrc src/app/app_diagnostics.cpp src/core/logging/run_log.cpp tests/app_diagnostics_tests.cpp -o "$output/app-diagnostics" >"$output/app-diagnostics.build.log" 2>&1
"$output/app-diagnostics" | tee "$output/app-diagnostics.log"
# Real operator-new fault injection runs without sanitizer allocator replacement.
g++ -std=c++17 -Wall -Wextra -Werror -pthread -Itests/stubs/app_diagnostics -Isrc \
    src/core/logging/run_log.cpp src/app/app_diagnostics.cpp tests/telemetry_allocation_failure_tests.cpp \
    -o "$output/allocation-failure" >"$output/allocation-failure.build.log" 2>&1
"$output/allocation-failure" | tee "$output/allocation-failure.log"
python3 -B -m unittest discover -s tools/diagnostics -p 'test_*.py' >"$output/b1-readers.log" 2>&1
python3 tools/check-architecture.py | tee "$output/architecture.log"
