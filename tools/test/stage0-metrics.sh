#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread -Isrc tests/stage0_metrics_tests.cpp -o work/stage0_metrics_tests
./work/stage0_metrics_tests | tee work/logs/stage0-metrics-tests.log
