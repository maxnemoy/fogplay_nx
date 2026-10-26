#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -Wall -Wextra -Werror -Itools/moonlight/compat \
    tools/moonlight-probe/tests/diagnostics_test.cpp -o work/moonlight-probe-diagnostics-test
./work/moonlight-probe-diagnostics-test
g++ -std=c++17 -Wall -Wextra -Werror -pthread -Ithird_party/moonlight/switch/include \
    tools/moonlight-probe/probe_buffers_tests.cpp -o work/moonlight-probe-buffers-test
./work/moonlight-probe-buffers-test
g++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Ithird_party/moonlight/switch/include -Ithird_party/moonlight/switch/include/crypto -Ithird_party \
    tools/moonlight-probe/host_client.cpp tools/moonlight-probe/tests/host_client_test.cpp -o work/moonlight-probe-host-test
./work/moonlight-probe-host-test
