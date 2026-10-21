#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/packet046
ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=size=640x360:rate=30 -frames:v 30 -c:v libx264 -preset ultrafast -tune zerolatency -x264-params aud=1:keyint=5:min-keyint=5:scenecut=0 -f h264 -y work/packet046/rtp-media.h264
g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread -Isrc $(pkg-config --cflags sdl2 libavcodec libavutil libswscale opus) src/features/streaming/infrastructure/media.cpp tests/rtp_media_tests.cpp $(pkg-config --libs sdl2 libavcodec libavutil libswscale opus) -o work/packet046/rtp_media_tests
./work/packet046/rtp_media_tests work/packet046/rtp-media.h264 | tee work/packet046/rtp-media-tests.log
