#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread -Isrc tests/control_delivery_tests.cpp -o work/control_delivery_tests
./work/control_delivery_tests | tee work/logs/control-delivery-tests.log
