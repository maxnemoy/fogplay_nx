#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
output=work/media-telemetry
if [[ -n "${FOGPLAY_DIAGNOSTIC_PROFILE:-}" ]]; then output+="-profile${FOGPLAY_DIAGNOSTIC_PROFILE}"; fi
if [[ "${SANITIZE:-0}" == 1 ]]; then output+=-asan; fi
mkdir -p "$output" work/logs
flags=(-std=c++17 -O2 -Wall -Wextra -Werror -pthread -g -Isrc)
if [[ -n "${FOGPLAY_DIAGNOSTIC_PROFILE:-}" ]]; then flags+=("-DFOGPLAY_DIAGNOSTIC_PROFILE=${FOGPLAY_DIAGNOSTIC_PROFILE}"); fi
if [[ "${SANITIZE:-0}" == 1 ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie); fi
g++ "${flags[@]}" tests/media_telemetry_stage_tests.cpp -o "$output/stage_tests"
timeout 20s "./$output/stage_tests"
g++ "${flags[@]}" tests/video_watchdog_tests.cpp -o "$output/watchdog_tests"
timeout 20s "./$output/watchdog_tests"
g++ "${flags[@]}" tests/media_telemetry_format_tests.cpp -o "$output/format_tests"
timeout 20s "./$output/format_tests"
ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=size=1280x720:rate=30 -frames:v 12 \
    -c:v libx264 -preset ultrafast -tune zerolatency -f h264 -y "$output/frames.h264"
for depth in 1 3; do
    g++ "${flags[@]}" -DFOGPLAY_MEDIA_READY_DEPTH="$depth" \
        $(pkg-config --cflags sdl2 libavcodec libavutil libswscale opus) \
        src/features/streaming/infrastructure/media.cpp tests/media_worker_tests.cpp \
        $(pkg-config --libs sdl2 libavcodec libavutil libswscale opus) \
        -Wl,--wrap=avcodec_open2,--wrap=avcodec_send_packet,--wrap=sws_scale,--wrap=SDL_UpdateYUVTexture,--wrap=opus_decode,--wrap=SDL_QueueAudio,--wrap=SDL_OpenAudioDevice,--wrap=SDL_PauseAudioDevice,--wrap=snprintf,--wrap=__snprintf_chk,--wrap=SDL_GetQueuedAudioSize,--wrap=av_get_pix_fmt_name \
        -o "$output/worker_tests_$depth"
    timeout 30s "./$output/worker_tests_$depth" "$output/frames.h264" telemetry-only
done
