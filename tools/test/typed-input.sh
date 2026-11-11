#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
if ! python3 tools/rtc/manifest.py check host; then
    bash tools/rtc/build.sh host
fi
mkdir -p work/logs
flags=(-std=c++17 -Wall -Wextra -Werror -pthread -DRTC_ENABLE_MEDIA=1 -DRTC_ENABLE_WEBSOCKET=1
    -DFOGPLAY_TYPED_INPUT=1 -DFOG_STREAM_TESTING -Isrc -Ithird_party -Ithird_party/rtc/host/include)
libs=(-Lthird_party/rtc/host/lib -Wl,--start-group -ldatachannel -ljuice -lusrsctp -lsrtp2
    -lmbedtls -lmbedx509 -lmbedcrypto -leverest -lp256m -Wl,--end-group)
g++ "${flags[@]}" src/features/streaming/infrastructure/stream.cpp \
    src/features/streaming/infrastructure/stream_protocol.cpp src/features/streaming/application/remote_input.cpp \
    tests/stream_integration.cpp -Wl,--wrap=sendto "${libs[@]}" -o work/stream_integration_typed
timeout 60 ./work/stream_integration_typed | tee work/logs/stream-integration-typed.log
