#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/media0413
ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=size=640x360:rate=30 -frames:v 12 -c:v libx264 -preset ultrafast -tune zerolatency -f h264 -y work/media0413/batch.h264
ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=size=640x480:rate=30 -frames:v 6 -c:v libx264 -preset ultrafast -tune zerolatency -f h264 -y work/media0413/changed.h264
g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread -Isrc $(pkg-config --cflags sdl2 libavcodec libavutil libswscale opus) src/features/streaming/infrastructure/media.cpp tests/media_presentation_tests.cpp $(pkg-config --libs sdl2 libavcodec libavutil libswscale opus) -Wl,--wrap=SDL_CreateTexture -Wl,--wrap=SDL_UpdateYUVTexture -Wl,--wrap=avcodec_receive_frame -Wl,--wrap=av_hwframe_transfer_data -o work/media0413/media_presentation_tests
./work/media0413/media_presentation_tests work/media0413/batch.h264 work/media0413/changed.h264 2>&1 | tee work/media0413/presentation-tests.log
