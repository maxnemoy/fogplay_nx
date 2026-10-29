#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
flags=()
suffix=""
if [[ "${1:-}" == "--tsan" ]]; then
    flags=(-fsanitize=thread -g -O1)
    suffix="-tsan"
elif [[ $# -ne 0 ]]; then
    printf 'Usage: %s [--tsan]\n' "$0" >&2
    exit 2
fi
g++ -std=c++17 -Wall -Wextra -Werror -pthread "${flags[@]}" \
    -Itests/stubs/app_diagnostics -Isrc src/app/app_diagnostics.cpp \
    src/core/logging/run_log.cpp tests/app_diagnostics_tests.cpp -o "work/app_diagnostics_tests$suffix"
"./work/app_diagnostics_tests$suffix" | tee "work/logs/app-diagnostics-tests$suffix.log"
