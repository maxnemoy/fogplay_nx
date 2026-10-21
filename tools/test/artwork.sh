#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -Wall -Wextra -Werror -DARTWORK_TEST_WRITE_FAULTS -pthread -Isrc $(pkg-config --cflags SDL2_image sdl2 libwebp) \
    src/features/catalog/infrastructure/artwork.cpp src/core/network/http.cpp tests/artwork_tests.cpp tests/no_replace_rename.cpp \
    -Wl,--wrap=rename,--wrap=write,--wrap=SDL_CreateTextureFromSurface $(pkg-config --libs SDL2_image sdl2 libwebp) -lcurl -o work/artwork_tests
SDL_VIDEODRIVER=dummy timeout 30 ./work/artwork_tests "${@}" | tee work/logs/artwork-tests.log
