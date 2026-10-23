#!/usr/bin/env bash
set -Eeuo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PLATFORM=${1:-switch}
case "$PLATFORM" in switch|host) ;; *) echo 'Usage: build.sh [switch|host]' >&2; exit 2;; esac
BUILD="$ROOT/work/rtc/build-$PLATFORM"
PREFIX="$ROOT/third_party/rtc/$PLATFORM"
mkdir -p "$ROOT/work/rtc" "$PREFIX/include" "$PREFIX/lib"
LOG="$ROOT/work/rtc/fetch-$PLATFORM.log"
trap 'status=$?; printf "RTC %s build failed (exit %s). Log: %s\n" "$PLATFORM" "$status" "$LOG" >&2; tail -n 30 "$LOG" >&2 || true; exit "$status"' ERR
"$ROOT/tools/rtc/fetch.sh" > "$LOG" 2>&1
# Never retain a successful stamp after a partial or failed rebuild.
rm -f "$PREFIX/build-manifest.json"
ARGS=(-DCMAKE_BUILD_TYPE=Release)
if [ "$PLATFORM" = switch ]; then
  export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
  ARGS+=(-DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake")
fi
LOG="$ROOT/work/rtc/configure-$PLATFORM.log"
cmake -S "$ROOT/tools/rtc" -B "$BUILD" "${ARGS[@]}" > "$LOG" 2>&1
LOG="$ROOT/work/rtc/build-$PLATFORM.log"
cmake --build "$BUILD" --target rtc-link-smoke -j "${JOBS:-8}" > "$LOG" 2>&1
cp -a "$ROOT/third_party/rtc/libdatachannel/include/rtc" "$PREFIX/include/"
cp -a "$ROOT/third_party/rtc/libdatachannel/deps/libjuice/include/juice" "$PREFIX/include/"
cp -a "$ROOT/third_party/rtc/mbedtls/include/mbedtls" "$ROOT/third_party/rtc/mbedtls/include/psa" "$PREFIX/include/"
cp "$ROOT/tools/rtc/mbedtls-user-config.h" "$PREFIX/include/"
cp "$BUILD/libdatachannel/libdatachannel-static.a" "$PREFIX/lib/libdatachannel.a"
cp "$BUILD/libdatachannel/deps/libjuice/libjuice-static.a" "$PREFIX/lib/libjuice.a"
cp "$BUILD/libdatachannel/deps/usrsctp/usrsctplib/libusrsctp.a" "$PREFIX/lib/"
cp "$BUILD/libdatachannel/deps/libsrtp/libsrtp2.a" "$PREFIX/lib/"
cp "$BUILD/mbedtls/library/"*.a "$PREFIX/lib/"
cp "$BUILD/mbedtls/3rdparty/everest/libeverest.a" "$BUILD/mbedtls/3rdparty/p256-m/libp256m.a" "$PREFIX/lib/"
if [ "$PLATFORM" = host ]; then
  LOG="$ROOT/work/rtc/run-host.log"
  timeout 20 "$BUILD/rtc-link-smoke" > "$LOG" 2>&1
  LOG="$ROOT/work/rtc/loopback-build.log"
  cmake --build "$BUILD" --target rtc-host-loopback -j "${JOBS:-8}" > "$LOG" 2>&1
  LOG="$ROOT/work/rtc/loopback-run.log"
  timeout 25 "$BUILD/rtc-host-loopback" > "$LOG" 2>&1
  LOG="$ROOT/work/rtc/udp-budget-build.log"
  cmake --build "$BUILD" --target rtc-udp-budget-test rtc-ice-diagnostics-test rtc-dtls-queue-test rtc-dtls-diagnostics-test rtc-dtls-receive-test rtc-dtls-ingress-test rtc-media-send-diagnostics-test rtc-rtcp-feedback-test -j "${JOBS:-8}" > "$LOG" 2>&1
  LOG="$ROOT/work/rtc/udp-budget-run.log"
  timeout 10 "$BUILD/rtc-udp-budget-test" > "$LOG" 2>&1
  LOG="$ROOT/work/rtc/ice-diagnostics-run.log"
  timeout 10 "$BUILD/rtc-ice-diagnostics-test" > "$LOG" 2>&1
  LOG="$ROOT/work/rtc/dtls-queue-run.log"
  timeout 10 "$BUILD/rtc-dtls-queue-test" > "$LOG" 2>&1
  LOG="$ROOT/work/rtc/dtls-diagnostics-run.log"
  timeout 25 "$BUILD/rtc-dtls-diagnostics-test" > "$LOG" 2>&1
  LOG="$ROOT/work/rtc/dtls-receive-run.log"
  timeout 15 "$BUILD/rtc-dtls-receive-test" > "$LOG" 2>&1
  LOG="$ROOT/work/rtc/dtls-ingress-run.log"
  timeout 20 "$BUILD/rtc-dtls-ingress-test" > "$LOG" 2>&1
  LOG="$ROOT/work/rtc/media-send-diagnostics-run.log"
  timeout 20 "$BUILD/rtc-media-send-diagnostics-test" > "$LOG" 2>&1
  LOG="$ROOT/work/rtc/rtcp-feedback-run.log"
  timeout 20 "$BUILD/rtc-rtcp-feedback-test" > "$LOG" 2>&1
else
  LOG="$ROOT/work/rtc/undefined-switch.txt"
  "$DEVKITPRO/devkitA64/bin/aarch64-none-elf-nm" -u "$BUILD/rtc-link-smoke.elf" > "$LOG"
  [ ! -s "$LOG" ]
fi
LOG="$ROOT/work/rtc/manifest-$PLATFORM.log"
python3 "$ROOT/tools/rtc/manifest.py" write "$PLATFORM" > "$LOG" 2>&1
printf 'RTC %s build and smoke check passed. Prefix: %s\n' "$PLATFORM" "$PREFIX"
