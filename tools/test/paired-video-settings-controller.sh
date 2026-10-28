#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -Wall -Wextra -Werror -pthread -Itests/quality_controller_stubs -Isrc \
    src/features/settings/presentation/video_settings_controller.cpp \
    tests/paired_video_settings_controller_tests.cpp -o work/paired_video_settings_controller_tests
./work/paired_video_settings_controller_tests | tee work/logs/paired-video-settings-controller-tests.log
