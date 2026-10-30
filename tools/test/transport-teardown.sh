#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -Wall -Wextra -Werror -pthread -Isrc \
    tests/transport_teardown_tests.cpp -o work/transport_teardown_tests
./work/transport_teardown_tests | tee work/logs/transport-teardown-tests.log
