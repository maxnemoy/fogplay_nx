#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
rtc=0
if [[ "${1:-}" == --rtc && $# -eq 1 ]]; then rtc=1
elif [[ $# -ne 0 ]]; then printf 'Usage: %s [--rtc]\n' "$0" >&2;exit 2
fi
mkdir -p work/logs
output=$(mktemp -d "$PWD/work/logs/v2-telemetry-network-XXXXXX")
printf 'Telemetry T2 logs: %s\n' "$output"
flags=(-std=c++17 -Wall -Wextra -Werror -pthread -g -fsanitize=address,undefined -fno-omit-frame-pointer)
g++ "${flags[@]}" -Isrc -Ithird_party tests/telemetry_network_tests.cpp -o "$output/numeric" >"$output/numeric.build.log" 2>&1
"$output/numeric" | tee "$output/numeric.log"
g++ "${flags[@]}" -Itests/stubs/telemetry_network -Isrc tests/telemetry_rtcp_cache_tests.cpp -o "$output/rtcp-cache" >"$output/rtcp-cache.build.log" 2>&1
"$output/rtcp-cache" | tee "$output/rtcp-cache.log"
for name in h264_rtp rtp_repair incoming_control incoming_control_diagnostics server_performance transport_history; do
    g++ "${flags[@]}" -Isrc -Ithird_party "tests/${name}_tests.cpp" -o "$output/$name" >"$output/$name.build.log" 2>&1
    "$output/$name" | tee "$output/$name.log"
done
python3 -B -m unittest discover -s tools/diagnostics -p 'test_*.py' >"$output/b1-readers.log" 2>&1
python3 tools/check-architecture.py | tee "$output/architecture.log"

if [[ "$rtc" == 1 ]]; then
    if [[ ! -f third_party/rtc/host/lib/libdatachannel.a ]]; then
        printf 'RTC host archive missing; run normal tools/rtc/build.sh host setup before --rtc.\n' >&2;exit 1
    fi
    libraries=(-Lthird_party/rtc/host/lib -Wl,--start-group -ldatachannel -ljuice -lusrsctp -lsrtp2 -lmbedtls -lmbedx509 -lmbedcrypto -leverest -lp256m -Wl,--end-group)
    # This unchanged RTC fixture has one pre-existing misleading-indentation
    # warning. Keep it visible without altering another owner's fixture source.
    g++ "${flags[@]}" -Wno-error=misleading-indentation -DRTC_ENABLE_MEDIA=1 -DRTC_ENABLE_WEBSOCKET=1 \
        -Isrc -Ithird_party/rtc/host/include tools/rtc/rtcp-feedback-test.cpp "${libraries[@]}" \
        -o "$output/rtcp-real" >"$output/rtcp-real.build.log" 2>&1
    timeout 30 "$output/rtcp-real" | tee "$output/rtcp-real.log"
fi
