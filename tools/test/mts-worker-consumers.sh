#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
sanitizers=(-fsanitize=address,undefined)
suffix="-asan"
if [[ "${1:-}" == "--tsan" ]]; then sanitizers=(-fsanitize=thread); suffix="-tsan"; fi
flags=(-std=c++17 -Wall -Wextra -Werror -g -pthread -DRTC_ENABLE_MEDIA=1 -DRTC_ENABLE_WEBSOCKET=1 -DFOG_STREAM_TESTING -Isrc -Ithird_party -Ithird_party/rtc/host/include)
libs=(-Lthird_party/rtc/host/lib -Wl,--start-group -ldatachannel -ljuice -lusrsctp -lsrtp2 -lmbedtls -lmbedx509 -lmbedcrypto -leverest -lp256m -Wl,--end-group)
g++ "${flags[@]}" "${sanitizers[@]}" src/features/streaming/infrastructure/stream.cpp \
    src/features/streaming/infrastructure/stream_protocol.cpp src/features/streaming/application/remote_input.cpp \
    tests/stream_integration.cpp -Wl,--wrap=sendto "${libs[@]}" -o "work/mts-worker-consumers${suffix}"
timeout 45 "./work/mts-worker-consumers${suffix}" --media-workers-only 2>&1 | tee "work/logs/mts-worker-consumers${suffix}.log"
