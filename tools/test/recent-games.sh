#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -Wall -Wextra -Werror -pthread -Isrc -Ithird_party \
    $(pkg-config --cflags SDL2_image sdl2) src/features/catalog/infrastructure/mts_catalog_mapper.cpp src/features/catalog/infrastructure/recent_games.cpp src/core/storage/storage.cpp src/features/catalog/infrastructure/artwork.cpp src/core/network/http.cpp \
    tests/recent_games_tests.cpp $(pkg-config --libs SDL2_image sdl2) -lwebp -lcurl -Wl,--wrap=rename,--wrap=fsync -o work/recent_games_tests
./work/recent_games_tests | tee work/logs/recent-games-tests.log
