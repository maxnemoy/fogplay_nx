#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
read -r -a cflags <<< "$(pkg-config --cflags sdl2 SDL2_ttf SDL2_image)"
read -r -a libs <<< "$(pkg-config --libs sdl2 SDL2_ttf SDL2_image)"
for preview in ui_preview stream_overlay_preview; do
    g++ -std=c++17 -Wall -Wextra -Werror -UNDEBUG -Isrc -Ithird_party "${cflags[@]}" \
        src/app/presentation/ui.cpp src/features/streaming/presentation/stream_overlay.cpp \
        "tests/$preview.cpp" "${libs[@]}" -o "work/$preview"
    SDL_VIDEODRIVER=dummy timeout 30 "work/$preview" "work/previews/$preview" \
        2>&1 | tee "work/logs/$preview.log"
done
