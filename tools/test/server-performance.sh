#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -Wall -Wextra -Werror -UNDEBUG -Isrc -Ithird_party \
    tests/server_performance_tests.cpp -o work/server_performance_tests
./work/server_performance_tests | tee work/logs/server-performance-tests.log
