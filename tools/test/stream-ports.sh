#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
output=$(mktemp -d "$PWD/work/logs/v2-stream-ports-XXXXXX")
printf 'Stream port logs: %s\n' "$output"
g++ -std=c++17 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc \
    tests/stream_ports_tests.cpp -o "$output/contracts" > "$output/contracts.build.log" 2>&1
"$output/contracts" | tee "$output/contracts.log"
for header in gamepad stream_connection playback_observation stream_input_sink stream_lifecycle; do
    printf '#include "features/streaming/domain/%s.hpp"\n' "$header" | \
        g++ -std=c++17 -Wall -Wextra -Werror -Isrc -x c++ -fsyntax-only - \
        > "$output/header-$header.log" 2>&1
done
g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Ithird_party -fsyntax-only \
    tests/stream_ports_compatibility_tests.cpp > "$output/compatibility.log" 2>&1
# Focused consumer compilation must not regain the compatibility aggregate.
g++ -std=c++17 -Isrc -MM tests/stream_ports_tests.cpp > "$output/dependencies.log"
if grep -q 'streaming_client.hpp' "$output/dependencies.log"; then
    printf 'Focused port consumer unexpectedly depends on StreamingClient\n' >&2
    exit 1
fi
for mode in 0 1; do
    g++ -std=c++17 -Wall -Wextra -Werror -Isrc -DFOGPLAY_INPUT_CHANGE_ONLY="$mode" \
        tests/gamepad_schedule_tests.cpp -o "$output/schedule-$mode" > "$output/schedule-$mode.build.log" 2>&1
    "$output/schedule-$mode" | tee "$output/schedule-$mode.log"
done
g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Ithird_party -fsyntax-only \
    src/features/streaming/infrastructure/stream_protocol.cpp > "$output/protocol-syntax.log" 2>&1
g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Ithird_party \
    src/features/streaming/infrastructure/stream_protocol.cpp tests/hevc_protocol_tests.cpp \
    -o "$output/hevc-protocol" > "$output/hevc-protocol.build.log" 2>&1
"$output/hevc-protocol" | tee "$output/hevc-protocol.log"
python3 tools/check-architecture.py | tee "$output/architecture.log"
printf 'Stream port contract, compatibility, focused includes and architecture checks passed\n'