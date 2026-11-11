#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
flags=(-std=c++17 -Wall -Wextra -Werror -pthread -Isrc)
if [[ ${SANITIZE:-0} == 1 ]]; then
    flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
fi
g++ "${flags[@]}" tests/network_event_batch_tests.cpp -o "$out/test"
"$out/test"
