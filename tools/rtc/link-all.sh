#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
BUILD="$ROOT/work/rtc/build-switch"
PREFIX="$ROOT/third_party/rtc/switch"
"$DEVKITPRO/devkitA64/bin/aarch64-none-elf-g++" \
  -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE \
  -specs="$DEVKITPRO/libnx/switch.specs" \
  "$BUILD/CMakeFiles/rtc-link-smoke.dir/link-smoke.o" \
  -Wl,--whole-archive "$PREFIX/lib/"*.a -Wl,--no-whole-archive \
  -L"$DEVKITPRO/portlibs/switch/lib" -L"$DEVKITPRO/libnx/lib" -lcurl -lz -lnx -lm \
  -o "$ROOT/work/rtc/link-all-switch.elf" > "$ROOT/work/rtc/link-all-switch.log" 2>&1
"$DEVKITPRO/devkitA64/bin/aarch64-none-elf-nm" -u "$ROOT/work/rtc/link-all-switch.elf" > "$ROOT/work/rtc/link-all-undefined.txt"
[ ! -s "$ROOT/work/rtc/link-all-undefined.txt" ]
printf 'All RTC archive objects link for Switch with zero undefined symbols.\n'
