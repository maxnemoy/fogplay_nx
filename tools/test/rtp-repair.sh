#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined -Isrc \
    tests/rtp_repair_tests.cpp -o work/rtp_repair_tests
./work/rtp_repair_tests | tee work/logs/rtp-repair-tests.log
