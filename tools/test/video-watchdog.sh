#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread -Isrc tests/video_watchdog_tests.cpp -o work/video_watchdog_tests
./work/video_watchdog_tests | tee work/logs/video-watchdog-tests.log
