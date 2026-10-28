#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
python3 tools/moonlight/tests/core_diagnostics_test.py | tee work/logs/moonlight-core-diagnostics-test.log
g++ -std=c++17 -Wall -Wextra -Werror -Isrc tools/moonlight/tests/app_selection_test.cpp \
    -o work/moonlight-app-selection-test
./work/moonlight-app-selection-test | tee work/logs/moonlight-app-selection-test.log
g++ -std=c++17 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare \
    -Itools/moonlight/tests/stubs -Itools/moonlight/compat -Itools/moonlight -Isrc \
    -Ithird_party/moonlight/switch/include \
    -idirafter "${DEVKITPRO:-/opt/devkitpro}/portlibs/switch/include" \
    -Ithird_party/moonlight/prepared/libgamestream -Ithird_party/moonlight/prepared/crypto \
    third_party/moonlight/prepared/libgamestream/xml.cpp third_party/moonlight/prepared/crypto/Data.cpp \
    tools/moonlight/tests/xml_diagnostics_test.cpp -l:libexpat.so.1 -o work/moonlight-xml-diagnostics-test
./work/moonlight-xml-diagnostics-test | tee work/logs/moonlight-xml-diagnostics-test.log
g++ -std=c++17 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -DUSE_MBEDTLS_CRYPTO=1 -Ithird_party/rtc/host/include \
    -Itools/moonlight/tests/stubs -Itools/moonlight/compat -Itools/moonlight -Isrc \
    -Ithird_party/moonlight/switch/include \
    -idirafter "${DEVKITPRO:-/opt/devkitpro}/portlibs/switch/include" \
    -Ithird_party/moonlight/prepared/libgamestream -Ithird_party/moonlight/prepared/crypto \
    third_party/moonlight/prepared/libgamestream/client.cpp \
    third_party/moonlight/prepared/libgamestream/xml.cpp third_party/moonlight/prepared/crypto/Data.cpp \
    tools/moonlight/tests/command_ack_test.cpp -l:libexpat.so.1 -o work/moonlight-command-ack-test
./work/moonlight-command-ack-test | tee work/logs/moonlight-command-ack-test.log
g++ -std=c++17 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Itools/moonlight/tests/stubs -Itools/moonlight/compat -Itools/moonlight -Isrc -Ithird_party/moonlight/switch/include \
    -Ithird_party/moonlight/switch/include/crypto \
    src/features/streaming/infrastructure/moonlight/moonlight_session.cpp \
    src/features/streaming/application/remote_input.cpp tools/moonlight/tests/session_test.cpp \
    -o work/moonlight-production-session-test
./work/moonlight-production-session-test | tee work/logs/moonlight-production-session-test.log
