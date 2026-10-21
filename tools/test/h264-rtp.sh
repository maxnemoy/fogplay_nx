#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined -Isrc \
    tests/h264_rtp_tests.cpp -o work/h264_rtp_tests
./work/h264_rtp_tests | tee work/logs/h264-rtp-tests.log
