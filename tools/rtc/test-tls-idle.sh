#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
mkdir -p work/tls-idle
# Fresh ephemeral certificates prevent stale one-day fixtures from failing later runs.
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=localhost -addext subjectAltName=DNS:localhost -keyout work/tls-idle/localhost.key -out work/tls-idle/localhost.pem > work/tls-idle/certificate-create.log 2>&1
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=untrusted -keyout work/tls-idle/untrusted.key -out work/tls-idle/untrusted.pem > work/tls-idle/untrusted-create.log 2>&1
g++ -std=c++20 -Wall -Wextra -Werror -pthread -DRTC_ENABLE_MEDIA=1 -DRTC_ENABLE_WEBSOCKET=1 -Ithird_party/rtc/host/include tools/rtc/tls-idle-regression.cpp -Lthird_party/rtc/host/lib -Wl,--start-group -ldatachannel -ljuice -lusrsctp -lsrtp2 -lmbedtls -lmbedx509 -lmbedcrypto -leverest -lp256m -Wl,--end-group -o work/tls-idle/regression
rm -f work/tls-idle/rejection-port.txt
python3 tools/rtc/tls-rejection-server.py work/tls-idle/localhost.pem work/tls-idle/localhost.key work/tls-idle/rejection-port.txt > work/tls-idle/rejection-server.log 2>&1 &
rejection_pid=$!
trap 'kill "$rejection_pid" 2>/dev/null || true' EXIT
for ((attempt=0; attempt<100; ++attempt)); do
    [ ! -f work/tls-idle/rejection-port.txt ] || break
    sleep 0.02
done
[ -f work/tls-idle/rejection-port.txt ]
rejection_port=$(cat work/tls-idle/rejection-port.txt)
timeout 40 work/tls-idle/regression work/tls-idle/localhost.pem work/tls-idle/localhost.key work/tls-idle/untrusted.pem "$rejection_port"
wait "$rejection_pid"
