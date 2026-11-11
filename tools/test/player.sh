#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/tests work/logs
flags=(-std=c++17 -Wall -Wextra -Werror -pthread -g -Isrc -Itests)
if [[ "${SANITIZE:-0}" == 1 ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
g++ "${flags[@]}" src/features/streaming/application/player.cpp tests/player_tests.cpp -o work/tests/player_tests
timeout 40s ./work/tests/player_tests
g++ -Itests/player_media_adapter_stubs "${flags[@]}" src/features/streaming/infrastructure/player_media.cpp \
    tests/player_media_adapter_tests.cpp -o work/tests/player_media_adapter_tests
timeout 20s ./work/tests/player_media_adapter_tests
python3 tests/player_rental_bridge_tests.py
