#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/tests work/logs
flags=(-std=c++17 -Wall -Wextra -Werror -pthread -g -Isrc)
if [[ "${SANITIZE:-0}" == 1 ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
g++ "${flags[@]}" src/features/session/application/session.cpp src/features/session/application/checkout.cpp \
    src/features/session/application/paired_checkout.cpp src/features/session/application/rental_session_coordinator.cpp \
    tests/rental_session_coordinator_tests.cpp -o work/tests/rental_session_coordinator_tests
timeout 30s ./work/tests/rental_session_coordinator_tests
