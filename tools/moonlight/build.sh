#!/usr/bin/env bash
set -Eeuo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
BUILD="$ROOT/work/moonlight/build-switch"
PREFIX="$ROOT/third_party/moonlight/switch"
mkdir -p "$ROOT/work/moonlight" "$PREFIX/lib" "$PREFIX/include/gamestream" "$PREFIX/include/crypto"
LOG="$ROOT/work/moonlight/fetch.log"
trap 'status=$?; printf "Moonlight build failed (%s). Log: %s\n" "$status" "$LOG" >&2; tail -n 40 "$LOG" >&2 || true; exit "$status"' ERR
bash "$ROOT/tools/moonlight/fetch.sh" > "$LOG" 2>&1
rm -f "$PREFIX/build-manifest.json"
LOG="$ROOT/work/moonlight/curl-build.log"
bash "$ROOT/tools/moonlight/build-curl.sh" > "$LOG" 2>&1
LOG="$ROOT/work/moonlight/configure.log"
cmake -S "$ROOT/tools/moonlight" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" > "$LOG" 2>&1
LOG="$ROOT/work/moonlight/build.log"
cmake --build "$BUILD" --target moonlight-link-smoke moonlight-curl-coexist-smoke -j "${JOBS:-8}" > "$LOG" 2>&1
"$DEVKITPRO/devkitA64/bin/aarch64-none-elf-nm" -u "$BUILD/moonlight-link-smoke.elf" > "$ROOT/work/moonlight/undefined.txt"
test ! -s "$ROOT/work/moonlight/undefined.txt"
"$DEVKITPRO/devkitA64/bin/aarch64-none-elf-nm" -u "$BUILD/moonlight-curl-coexist-smoke.elf" > "$ROOT/work/moonlight/coexist-undefined.txt"
test ! -s "$ROOT/work/moonlight/coexist-undefined.txt"
"$DEVKITPRO/devkitA64/bin/aarch64-none-elf-nm" --defined-only "$BUILD/moonlight-curl-coexist-smoke.elf" > "$ROOT/work/moonlight/coexist-symbols.txt"
# Both entry points must be present; a successful link alone could select one curl.
# System portlibs may use LTO and internalize these functions at final link.
grep -Eq ' [Tt] curl_easy_init$' "$ROOT/work/moonlight/coexist-symbols.txt"
grep -Eq ' [Tt] fog_moonlight_curl_easy_init$' "$ROOT/work/moonlight/coexist-symbols.txt"
grep -Eq ' [Tt] Curl_cookie_init$' "$ROOT/work/moonlight/coexist-symbols.txt"
grep -Eq ' [DdRr] Curl_ssl_libnx$' "$ROOT/work/moonlight/coexist-symbols.txt"
grep -Eq ' [DdRr] fog_moonlight_Curl_ssl_mbedtls$' "$ROOT/work/moonlight/coexist-symbols.txt"
cp "$BUILD/libgamestream.a" "$PREFIX/lib/"
cp "$BUILD/moonlight-common-c/libmoonlight-common-c.a" "$PREFIX/lib/"
cp "$BUILD/moonlight-common-c/enet/libenet.a" "$PREFIX/lib/"
cp "$ROOT/third_party/moonlight/prepared/libgamestream/"*.h "$PREFIX/include/gamestream/"
cp "$ROOT/third_party/moonlight/prepared/crypto/"*.hpp "$PREFIX/include/crypto/"
cp "$ROOT/third_party/moonlight/moonlight-switch/extern/moonlight-common-c/src/Limelight.h" "$PREFIX/include/"
cp "$ROOT/tools/moonlight/FogMoonlightDiagnostics.h" "$PREFIX/include/"
cp "$ROOT/tools/moonlight/compat/Settings.hpp" "$PREFIX/include/"
cp "$ROOT/tools/moonlight/compat/Diagnostics.hpp" "$PREFIX/include/"
python3 "$ROOT/tools/moonlight/manifest.py" "$PREFIX"
printf 'Moonlight Switch static libraries and complete link check passed: %s\n' "$PREFIX"
