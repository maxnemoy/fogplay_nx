#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/steam0414
flags=(-std=c++17 -Wall -Wextra -Werror -pthread -Isrc -Ithird_party)
output=work/steam0414/steam_artwork_tests
if [[ "${1:-}" == --asan ]]; then
    flags+=(-g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined)
    output+=-asan
fi
g++ "${flags[@]}" src/features/catalog/infrastructure/steam_artwork.cpp src/core/network/http.cpp src/core/storage/storage.cpp \
    tests/steam_artwork_tests.cpp -lcurl -o "$output"
timeout 45 "$output" | tee "${output}.log"