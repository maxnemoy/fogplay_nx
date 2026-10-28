#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    -Itests/stubs/rumble -Isrc src/platform/switch/rumble.cpp tests/rumble_tests.cpp \
    -o work/rumble_tests
./work/rumble_tests | tee work/logs/rumble-tests.log
