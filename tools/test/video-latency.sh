#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined -Isrc tests/video_latency_tests.cpp -o work/video_latency_tests
./work/video_latency_tests | tee work/logs/video-latency-tests.log
