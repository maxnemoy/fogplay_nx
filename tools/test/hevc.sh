#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/hevc047
g++ -std=c++17 -Wall -Wextra -Werror -Isrc tests/hevc_rtp_tests.cpp -o work/hevc047/hevc_rtp_tests
./work/hevc047/hevc_rtp_tests | tee work/hevc047/rtp-tests.log
g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Ithird_party src/features/streaming/infrastructure/stream_protocol.cpp tests/hevc_protocol_tests.cpp -o work/hevc047/hevc_protocol_tests
./work/hevc047/hevc_protocol_tests | tee work/hevc047/protocol-tests.log
ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=size=640x360:rate=30 -frames:v 30 -c:v libx265 -preset ultrafast -tune zerolatency -x265-params aud=1:keyint=5:min-keyint=5:scenecut=0:open-gop=0:bframes=0:pools=1:frame-threads=1 -f hevc -y work/hevc047/rtp-media.hevc 2>work/hevc047/fixture.log
g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread -Isrc $(pkg-config --cflags sdl2 libavcodec libavutil libswscale opus) src/features/streaming/infrastructure/media.cpp tests/hevc_media_tests.cpp $(pkg-config --libs sdl2 libavcodec libavutil libswscale opus) -o work/hevc047/hevc_media_tests
./work/hevc047/hevc_media_tests work/hevc047/rtp-media.hevc | tee work/hevc047/media-tests.log
