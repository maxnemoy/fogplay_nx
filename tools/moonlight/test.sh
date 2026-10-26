#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -Wall -Wextra -Werror -Isrc tools/moonlight/tests/app_selection_test.cpp \
    -o work/moonlight-app-selection-test
./work/moonlight-app-selection-test | tee work/logs/moonlight-app-selection-test.log
g++ -std=c++17 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Itools/moonlight/tests/stubs -Isrc -Ithird_party/moonlight/switch/include \
    -Ithird_party/moonlight/switch/include/crypto \
    src/features/streaming/infrastructure/moonlight/moonlight_session.cpp \
    src/features/streaming/application/remote_input.cpp tools/moonlight/tests/session_test.cpp \
    -o work/moonlight-production-session-test
./work/moonlight-production-session-test | tee work/logs/moonlight-production-session-test.log
