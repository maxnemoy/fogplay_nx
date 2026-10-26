#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -Itests/stubs/ps_sensors -Isrc \
    src/platform/switch/ps_sensors.cpp tests/ps_sensors_tests.cpp \
    -o work/ps_sensors_tests
./work/ps_sensors_tests | tee work/logs/ps-sensors-tests.log
