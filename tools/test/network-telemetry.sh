#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
output=$(mktemp -d "$PWD/work/logs/v2-network-telemetry-XXXXXX")
printf 'Network telemetry logs: %s\n' "$output"
flags=(-std=c++17 -Wall -Wextra -Werror -pthread -g -fsanitize=address,undefined -fno-omit-frame-pointer)
g++ "${flags[@]}" -Isrc -Ithird_party tests/network_telemetry_records_tests.cpp src/core/logging/run_log.cpp -o "$output/records" >"$output/records.build.log" 2>&1
FOG_NETWORK_FIXTURE="$output/b1.log" "$output/records" | tee "$output/records.log"
python3 -B tools/test/network-telemetry-b1.py "$output/b1.log" "$output/b1.json" | tee "$output/b1-typed.log"
g++ -std=c++17 -Wall -Wextra -Werror -pthread -Isrc \
    tests/network_telemetry_allocation_tests.cpp src/core/logging/run_log.cpp \
    -o "$output/allocation" >"$output/allocation.build.log" 2>&1
"$output/allocation" | tee "$output/allocation.log"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Itools/moonlight -c tools/moonlight/FogMoonlightDiagnostics.c -o "$output/core-diagnostics.o"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Itools/moonlight -c tools/moonlight/FogMoonlightTermination.c -o "$output/termination.o"
g++ "${flags[@]}" -Itools/moonlight/tests/stubs -Itools/moonlight/compat -Itools/moonlight -Isrc -Ithird_party/moonlight/switch/include -Ithird_party/moonlight/switch/include/crypto src/features/streaming/infrastructure/moonlight/moonlight_session.cpp src/features/streaming/application/remote_input.cpp tests/network_telemetry_moonlight_tests.cpp "$output/core-diagnostics.o" "$output/termination.o" src/core/logging/run_log.cpp -o "$output/fh" >"$output/fh.build.log" 2>&1
"$output/fh" | tee "$output/fh.log"
if [[ ! -f third_party/rtc/host/lib/libdatachannel.a ]]; then printf 'RTC host archive missing; run normal reviewed setup before this provider gate.\n' >&2;exit 1;fi
libraries=(-Lthird_party/rtc/host/lib -Wl,--start-group -ldatachannel -ljuice -lusrsctp -lsrtp2 -lmbedtls -lmbedx509 -lmbedcrypto -leverest -lp256m -Wl,--end-group)
g++ "${flags[@]}" -DFOG_STREAM_TESTING -DRTC_ENABLE_MEDIA=1 -DRTC_ENABLE_WEBSOCKET=1 -Isrc -Ithird_party -Ithird_party/rtc/host/include src/features/streaming/infrastructure/stream.cpp src/features/streaming/infrastructure/stream_protocol.cpp src/features/streaming/application/remote_input.cpp src/core/logging/run_log.cpp tests/network_telemetry_mts_tests.cpp -Wl,--wrap=sendto "${libraries[@]}" -o "$output/mts" >"$output/mts.build.log" 2>&1
timeout 30 "$output/mts" | tee "$output/mts.log"
g++ "${flags[@]}" -Isrc -Ithird_party/rtc/host/include tests/network_dtls_formatter_tests.cpp -o "$output/dtls" >"$output/dtls.build.log" 2>&1
"$output/dtls" | tee "$output/dtls.log"
g++ "${flags[@]}" -Isrc -Ithird_party/rtc/host/include tests/network_ice_formatter_tests.cpp -o "$output/ice" >"$output/ice.build.log" 2>&1
"$output/ice" | tee "$output/ice.log"
python3 -B -m unittest discover -s tools/diagnostics -p 'test_*.py' >"$output/b1-readers.log" 2>&1
python3 tools/check-architecture.py | tee "$output/architecture.log"
