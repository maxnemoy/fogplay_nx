#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
mkdir -p work/tests
g++ -std=c++17 -Wall -Wextra -Werror -Isrc tests/media_annexb_tests.cpp -o work/tests/media_annexb_tests
./work/tests/media_annexb_tests | tee work/tests/media-annexb-tests.log
ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=size=640x360:rate=30 -frames:v 3 -c:v libx264 -preset ultrafast -tune zerolatency -f h264 -y work/media-test.h264
g++ -std=c++17 -Wall -Wextra -Werror -Isrc $(pkg-config --cflags sdl2 libavcodec libavutil libswscale opus) src/features/streaming/infrastructure/media.cpp tests/media_tests.cpp $(pkg-config --libs sdl2 libavcodec libavutil libswscale opus) -o work/media_tests
ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=size=640x480:rate=30 -frames:v 3 -c:v libx264 -preset ultrafast -tune zerolatency -f h264 -y work/media-letterbox-test.h264
./work/media_tests work/media-test.h264 work/media-letterbox-test.h264 2>&1 | tee work/tests/media-tests.log
./work/media_tests work/media-letterbox-test.h264 work/media-test.h264 2>&1 | tee work/tests/media-letterbox-tests.log
g++ -std=c++17 -Wall -Wextra -Werror -Isrc $(pkg-config --cflags sdl2 libavcodec libavutil libswscale opus) \
    src/features/streaming/infrastructure/media.cpp tests/media_reset_failure_tests.cpp \
    $(pkg-config --libs sdl2 libavcodec libavutil libswscale opus) \
    -Wl,--wrap=avcodec_open2 -Wl,--wrap=SDL_OpenAudioDevice \
    -Wl,--wrap=SDL_CloseAudioDevice -Wl,--wrap=SDL_PauseAudioDevice -o work/media_reset_failure_tests
./work/media_reset_failure_tests 2>&1 | tee work/tests/media-reset-failure-tests.log
