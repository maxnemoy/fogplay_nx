#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
output=$(mktemp -d "$PWD/work/logs/v2-telemetry-providers-XXXXXX")
printf 'Telemetry provider logs: %s\n' "$output"
flags=(-std=c++17 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer)
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Itools/moonlight -c tools/moonlight/FogMoonlightTermination.c -o "$output/termination.o"
g++ "${flags[@]}" -Itools/moonlight/tests/stubs -Itools/moonlight/compat -Itools/moonlight -Isrc -Ithird_party/moonlight/switch/include -Ithird_party/moonlight/switch/include/crypto src/features/streaming/infrastructure/moonlight/moonlight_session.cpp src/features/streaming/application/remote_input.cpp tests/telemetry_moonlight_tests.cpp "$output/termination.o" -o "$output/fh" >"$output/fh.build.log" 2>&1
"$output/fh" | tee "$output/fh.log"
for options in full control encrypted; do
    definitions=()
    if [[ "$options" == control ]]; then definitions=(-DFOGPLAY_STAGE1_FEATURE_MASK=0); fi
    if [[ "$options" == encrypted ]]; then definitions=(-DFOGPLAY_MOONLIGHT_ENCRYPTION=1); fi
    g++ "${flags[@]}" "${definitions[@]}" -Itools/moonlight/tests/stubs -Itools/moonlight/compat -Itools/moonlight -Isrc -Ithird_party/moonlight/switch/include -Ithird_party/moonlight/switch/include/crypto src/features/streaming/infrastructure/moonlight/moonlight_session.cpp src/features/streaming/application/remote_input.cpp tools/moonlight/tests/session_test.cpp "$output/termination.o" -o "$output/fh-$options" >"$output/fh-$options.build.log" 2>&1
    "$output/fh-$options" | tee "$output/fh-$options.log"
done
# No dependency mutation or implicit download/build. Setup is the normal reviewed
# tools/rtc/build.sh host; the root integration gate verifies its manifest.
if [[ ! -f third_party/rtc/host/lib/libdatachannel.a ]]; then
    printf 'RTC host archive missing; run the normal tools/rtc/build.sh host setup first.\n' >&2; exit 1
fi
rtc=(-Lthird_party/rtc/host/lib -Wl,--start-group -ldatachannel -ljuice -lusrsctp -lsrtp2 -lmbedtls -lmbedx509 -lmbedcrypto -leverest -lp256m -Wl,--end-group)
g++ "${flags[@]}" -DFOG_STREAM_TESTING -DRTC_ENABLE_MEDIA=1 -DRTC_ENABLE_WEBSOCKET=1 -Isrc -Ithird_party -Ithird_party/rtc/host/include src/features/streaming/infrastructure/stream.cpp src/features/streaming/infrastructure/stream_protocol.cpp src/features/streaming/application/remote_input.cpp tests/telemetry_mts_tests.cpp -Wl,--wrap=sendto "${rtc[@]}" -o "$output/mts" >"$output/mts.build.log" 2>&1
timeout 30 "$output/mts" | tee "$output/mts.log"
