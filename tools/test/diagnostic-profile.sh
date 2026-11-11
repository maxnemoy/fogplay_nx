#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
for profile in 0 1 2; do
    g++ -std=c++17 -Wall -Wextra -Werror -Isrc -DFOGPLAY_DIAGNOSTIC_PROFILE="$profile" \
        tests/diagnostic_profile_tests.cpp -o "work/diagnostic-profile-$profile"
    "work/diagnostic-profile-$profile"
done
for invalid in -1 3; do
    if g++ -std=c++17 -Isrc -DFOGPLAY_DIAGNOSTIC_PROFILE="$invalid" -fsyntax-only \
        tests/diagnostic_profile_tests.cpp > "work/logs/diagnostic-profile-invalid-$invalid.log" 2>&1; then
        printf 'Invalid diagnostic profile unexpectedly accepted: %s\n' "$invalid" >&2
        exit 1
    fi
    grep -q 'FOGPLAY_DIAGNOSTIC_PROFILE must be' "work/logs/diagnostic-profile-invalid-$invalid.log"
done
