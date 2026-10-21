#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.."
mkdir -p work/av-test
runtime=work/av-test/venv/bin/python
if [[ ! -x "$runtime" ]]; then
    python3 -m venv work/av-test/venv
    work/av-test/venv/bin/pip install -r tools/av-test/pc/requirements.txt > work/av-test/pip.log 2>&1
fi
flags=(-std=c++17 -Wall -Wextra -Werror -pthread -DRTC_ENABLE_MEDIA=1 -DRTC_ENABLE_WEBSOCKET=1
       -Isrc -Ithird_party -Ithird_party/rtc/host/include)
libs=(-Lthird_party/rtc/host/lib -Wl,--start-group -ldatachannel -ljuice -lusrsctp -lsrtp2
      -lmbedtls -lmbedx509 -lmbedcrypto -leverest -lp256m -Wl,--end-group)
read -r -a media_flags <<< "$(pkg-config --cflags sdl2 libavcodec libavutil libswscale opus)"
read -r -a media_libs <<< "$(pkg-config --libs sdl2 libavcodec libavutil libswscale opus)"
g++ "${flags[@]}" "${media_flags[@]}" src/features/streaming/infrastructure/media.cpp tools/av-test/tests/interop.cpp \
    "${libs[@]}" "${media_libs[@]}" -o work/av-test/interop > work/av-test/interop-build.log 2>&1
port=${AV_TEST_PORT:-8766}
"$runtime" tools/av-test/pc/server.py --host 127.0.0.1 --port "$port" \
    --log-dir work/av-test/pc-logs > work/av-test/server-console.log 2>&1 &
server_pid=$!
stop_server() {
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
}
trap stop_server EXIT
for attempt in {1..100}; do
    kill -0 "$server_pid" || { cat work/av-test/server-console.log; exit 1; }
    if "$runtime" -c 'import urllib.request,sys; urllib.request.urlopen(sys.argv[1],timeout=0.2).read()' \
       "http://127.0.0.1:$port/status" 2>/dev/null; then break; fi
    sleep 0.1
done
timeout 50 work/av-test/interop "ws://127.0.0.1:$port/ws" | tee work/av-test/interop.log
