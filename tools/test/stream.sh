#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
if ! python3 tools/rtc/manifest.py check host; then
    bash tools/rtc/build.sh host
fi
mkdir -p work/logs
bash tools/test/stream-input.sh
bash tools/test/reconnect.sh
bash tools/test/server-performance.sh
bash tools/test/control-delivery.sh
flags=(-std=c++17 -Wall -Wextra -Werror -pthread -DRTC_ENABLE_MEDIA=1 -DRTC_ENABLE_WEBSOCKET=1 -Isrc -Ithird_party -Ithird_party/rtc/host/include)
libs=(-Lthird_party/rtc/host/lib -Wl,--start-group -ldatachannel -ljuice -lusrsctp -lsrtp2 -lmbedtls -lmbedx509 -lmbedcrypto -leverest -lp256m -Wl,--end-group)
g++ "${flags[@]}" tests/video_rtp_handler_tests.cpp "${libs[@]}" -o work/video_rtp_handler_tests
./work/video_rtp_handler_tests | tee work/logs/video-rtp-handler-tests.log
g++ "${flags[@]}" tests/incoming_control_tests.cpp -o work/incoming_control_tests
./work/incoming_control_tests | tee work/logs/incoming-control-tests.log
g++ "${flags[@]}" tests/incoming_control_diagnostics_tests.cpp -o work/incoming_control_diagnostics_tests
./work/incoming_control_diagnostics_tests | tee work/logs/incoming-control-diagnostics-tests.log
g++ "${flags[@]}" tests/transport_history_tests.cpp -o work/transport_history_tests
./work/transport_history_tests | tee work/logs/transport-history-tests.log
g++ "${flags[@]}" tests/route_snapshot_schedule_tests.cpp -o work/route_snapshot_schedule_tests
./work/route_snapshot_schedule_tests | tee work/logs/route-snapshot-schedule-tests.log
g++ "${flags[@]}" tests/stream_queue_tests.cpp -o work/stream_queue_tests
./work/stream_queue_tests | tee work/logs/stream-queue-tests.log
g++ "${flags[@]}" tests/auto_quality_tests.cpp -o work/auto_quality_tests
./work/auto_quality_tests | tee work/logs/auto-quality-tests.log
g++ "${flags[@]}" tests/resolution_scale_tests.cpp -o work/resolution_scale_tests
./work/resolution_scale_tests | tee work/logs/resolution-scale-tests.log
g++ "${flags[@]}" -DSTREAM_RTC_TESTS src/features/streaming/infrastructure/stream.cpp src/features/streaming/infrastructure/stream_protocol.cpp tests/stream_tests.cpp "${libs[@]}" -o work/stream_tests_rtc
./work/stream_tests_rtc | tee work/logs/stream-tests.log
g++ "${flags[@]}" -DFOG_STREAM_TESTING src/features/streaming/infrastructure/stream.cpp src/features/streaming/infrastructure/stream_protocol.cpp src/features/streaming/application/remote_input.cpp tests/stream_integration.cpp -Wl,--wrap=sendto "${libs[@]}" -o work/stream_integration
timeout 60 ./work/stream_integration | tee work/logs/stream-integration.log
