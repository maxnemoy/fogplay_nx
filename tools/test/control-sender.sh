#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -Wall -Wextra -Werror -pthread -DFOG_STREAM_TESTING -Isrc tests/control_sender_tests.cpp -o work/control_sender_tests
./work/control_sender_tests | tee work/logs/control-sender-tests.log
