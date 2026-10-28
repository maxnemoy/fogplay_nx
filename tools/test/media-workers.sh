#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/media-workers
ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=size=1280x720:rate=30 -frames:v 12 -c:v libx264 -preset ultrafast -tune zerolatency -f h264 -y work/media-workers/frames.h264
g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread -Isrc \
    $(pkg-config --cflags sdl2 libavcodec libavutil libswscale opus) \
    src/features/streaming/infrastructure/media.cpp tests/media_worker_tests.cpp \
    $(pkg-config --libs sdl2 libavcodec libavutil libswscale opus) \
    -Wl,--wrap=avcodec_open2,--wrap=avcodec_send_packet,--wrap=sws_scale,--wrap=SDL_UpdateYUVTexture,--wrap=opus_decode,--wrap=SDL_QueueAudio,--wrap=SDL_OpenAudioDevice \
    -o work/media-workers/media_worker_tests
timeout 30s ./work/media-workers/media_worker_tests work/media-workers/frames.h264 2>&1 | tee work/media-workers/tests.log
