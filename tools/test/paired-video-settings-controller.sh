#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/tests work/logs
flags=(-std=c++17 -Wall -Wextra -Werror -pthread -g -Itests/quality_controller_stubs -Itests -Isrc)
suffix=""
if [[ "${SANITIZE:-0}" == 1 ]]; then
    flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie)
    suffix="-sanitized"
fi
sources=(src/features/streaming/application/player.cpp
    src/features/session/application/session.cpp src/features/session/application/checkout.cpp
    src/features/session/application/paired_checkout.cpp src/features/session/application/rental_session_coordinator.cpp)
g++ "${flags[@]}" "${sources[@]}" src/features/settings/presentation/video_settings_controller.cpp \
    tests/paired_video_settings_controller_tests.cpp -o "work/tests/paired_video_settings_controller_tests${suffix}"
timeout 40s "./work/tests/paired_video_settings_controller_tests${suffix}" 2>&1 | tee "work/logs/paired-video-settings-controller-tests${suffix}.log"
g++ "${flags[@]}" "${sources[@]}" tests/paired_transport_recovery_tests.cpp \
    -o "work/tests/paired_transport_recovery_tests${suffix}"
timeout 40s "./work/tests/paired_transport_recovery_tests${suffix}" 2>&1 | tee "work/logs/paired-transport-recovery-tests${suffix}.log"
