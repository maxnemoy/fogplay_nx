#!/usr/bin/env bash
# Extended, local-only RTC regressions; intentionally opt-in from all.sh.
set -Eeuo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
log=work/logs/rtc-extended.log
trap 'status=$?; printf "Extended RTC tests failed (exit %s). Log: %s\n" "$status" "$log" >&2; tail -n 30 "$log" >&2 || true; exit "$status"' ERR
if ! python3 tools/rtc/manifest.py check host; then
    bash tools/rtc/build.sh host
fi
# Separate tree prevents the joinable-cleanup test flag changing normal host libs.
build=work/rtc/build-host-regressions
cmake -S tools/rtc -B "$build" -DCMAKE_BUILD_TYPE=Release -DRTC_JOINABLE_CLEANUP_TEST=ON > "$log" 2>&1
targets=(rtc-init-rollback-test rtc-loopback-failure-test rtc-wakeup-check-test
         rtc-numeric-layout-contract rtc-numeric-contract rtc-pipe-contract rtc-cleanup-thread-test)
cmake --build "$build" --target "${targets[@]}" -j "${JOBS:-8}" >> "$log" 2>&1
for target in "${targets[@]}"; do
    printf 'Running %s\n' "$target" | tee -a "$log"
    timeout 300 stdbuf -oL -eL "$build/$target" 2>&1 | tee -a "$log"
done
timeout 300 stdbuf -oL -eL "$build/rtc-cleanup-thread-test" --spawn-failure 2>&1 | tee -a "$log"
bash tools/rtc/test-tls-idle.sh >> "$log" 2>&1
printf 'Extended RTC lifecycle, contracts and TLS regressions passed.\n' | tee -a "$log"
