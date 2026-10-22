#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
sanitizers=(-fsanitize=address,undefined)
suffix=""
if [[ "${1:-}" == "--tsan" ]]; then
    sanitizers=(-fsanitize=thread)
    suffix="-tsan"
fi
g++ -std=c++17 -Wall -Wextra -Werror -g -pthread "${sanitizers[@]}" -Isrc \
    tests/timed_mutex_tests.cpp -o "work/timed_mutex_tests${suffix}"
"./work/timed_mutex_tests${suffix}" | tee "work/logs/timed-mutex-tests${suffix}.log"
