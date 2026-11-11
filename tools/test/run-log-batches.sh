#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
output=work/run-log-batches
if [[ "${SANITIZE:-0}" == 1 ]]; then output+=-asan; fi
mkdir -p "$output" work/logs
flags=(-std=c++17 -Wall -Wextra -Werror -pthread -g -Isrc)
sanitizer=()
if [[ "${SANITIZE:-0}" == 1 ]]; then sanitizer=(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie); fi
g++ "${flags[@]}" "${sanitizer[@]}" src/core/logging/run_log.cpp tests/run_log_batch_tests.cpp -Wl,--wrap=write -o "$output/batches"
timeout 30s "$output/batches"
# Real operator-new fault injection must keep the system allocator.
g++ "${flags[@]}" src/core/logging/run_log.cpp tests/run_log_batch_allocation_tests.cpp -o "$output/allocation"
timeout 30s "$output/allocation"
