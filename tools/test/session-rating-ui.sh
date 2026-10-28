#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/session-rating
g++ -std=c++17 -Wall -Wextra -Werror -pthread -Itests/rating_controller_stubs -Isrc \
    src/features/session/presentation/session_rating_controller.cpp tests/session_rating_controller_tests.cpp \
    -o work/session-rating/controller-tests
./work/session-rating/controller-tests | tee work/session-rating/controller.log
g++ -std=c++17 -Wall -Wextra -Werror -Isrc $(pkg-config --cflags sdl2 SDL2_ttf SDL2_image) \
    src/features/session/presentation/session_rating_view.cpp src/app/presentation/ui.cpp tests/session_rating_ui_tests.cpp \
    $(pkg-config --libs sdl2 SDL2_ttf SDL2_image) -o work/session-rating/presentation-tests
./work/session-rating/presentation-tests work/session-rating/preview | tee work/session-rating/presentation.log
python3 tests/software_keyboard_tests.py | tee work/session-rating/keyboard.log
