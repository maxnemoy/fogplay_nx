#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/tests
g++ -std=c++17 -Wall -Wextra -Werror -pthread -Isrc -Ithird_party \
    $(pkg-config --cflags SDL2_image sdl2 libwebp) \
    src/features/catalog/infrastructure/artwork.cpp src/core/network/http.cpp src/core/storage/storage.cpp src/features/catalog/infrastructure/steam_artwork.cpp \
    src/features/catalog/presentation/catalog_artwork.cpp src/features/catalog/infrastructure/recent_games.cpp tests/catalog_artwork_tests.cpp \
    $(pkg-config --libs SDL2_image sdl2 libwebp) -lcurl -o work/tests/catalog_artwork_tests
SDL_VIDEODRIVER=dummy timeout 30 ./work/tests/catalog_artwork_tests 2>&1 | tee work/tests/catalog-artwork-tests.log
