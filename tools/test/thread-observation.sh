#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
flags=()
suffix=""
if [[ "${1:-}" == "--tsan" ]]; then flags=(-fsanitize=thread -g -O1); suffix="-tsan"; fi
g++ -std=c++17 -Wall -Wextra -Werror -pthread "${flags[@]}" -Isrc tests/thread_observation_tests.cpp -o "work/thread_observation_tests$suffix"
"./work/thread_observation_tests$suffix" | tee "work/logs/thread-observation-tests$suffix.log"
