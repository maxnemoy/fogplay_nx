#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -Wall -Wextra -Werror -pthread -Isrc tests/media_signal_tests.cpp -o work/media_signal_tests
./work/media_signal_tests | tee work/logs/media-signals-tests.log
python3 tests/stage3_playback_glue_tests.py | tee work/logs/stage3-playback-glue-tests.log
