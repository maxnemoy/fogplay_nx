#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
sanitizers=(-fsanitize=address,undefined)
suffix=""
if [[ "${1:-}" == "--tsan" ]]; then sanitizers=(-fsanitize=thread); suffix="-tsan"; fi
g++ -std=c++17 -Wall -Wextra -Werror -g -pthread "${sanitizers[@]}" -Isrc \
    tests/spsc_video_queue_tests.cpp -o "work/spsc_video_queue_tests${suffix}"
"./work/spsc_video_queue_tests${suffix}" | tee "work/logs/spsc-video-queue-tests${suffix}.log"
