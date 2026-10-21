#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/ffmpeg-diag
g++ -std=c++17 -Wall -Wextra -Werror -pthread -Isrc tests/media_decoder_diagnostics_tests.cpp -o work/ffmpeg-diag/capture-tests
./work/ffmpeg-diag/capture-tests | tee work/ffmpeg-diag/capture-tests.log
g++ -std=c++17 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/media_decoder_diagnostics_tests.cpp -o work/ffmpeg-diag/capture-tests-sanitized
./work/ffmpeg-diag/capture-tests-sanitized | tee work/ffmpeg-diag/capture-tests-sanitized.log

g++ -std=c++17 -Wall -Wextra -Werror -Isrc $(pkg-config --cflags sdl2 libavcodec libavutil libswscale opus) src/features/streaming/infrastructure/media.cpp tests/decoder_logging_lifecycle_tests.cpp $(pkg-config --libs sdl2 libavcodec libavutil libswscale opus) -Wl,--wrap=av_log_set_callback,--wrap=SDL_OpenAudioDevice,--wrap=SDL_CloseAudioDevice,--wrap=SDL_PauseAudioDevice -o work/ffmpeg-diag/lifecycle-tests
./work/ffmpeg-diag/lifecycle-tests | tee work/ffmpeg-diag/lifecycle-tests.log
