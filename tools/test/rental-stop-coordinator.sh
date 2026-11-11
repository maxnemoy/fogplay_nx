#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/tests work/logs
flags=(-std=c++17 -Wall -Wextra -Werror -g -Isrc)
if [[ "${SANITIZE:-0}" == 1 ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
g++ "${flags[@]}" tests/rental_stop_coordinator_tests.cpp -o work/tests/rental_stop_coordinator_tests
./work/tests/rental_stop_coordinator_tests
