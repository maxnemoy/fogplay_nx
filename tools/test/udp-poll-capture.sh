#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
out=$(mktemp -d /tmp/fog-udp-capture-XXXXXX)
trap 'rm -rf "$out"' EXIT
g++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc -Itools/rtc/typed-ice tests/udp_poll_capture_tests.cpp -o "$out/test"
"$out/test"
