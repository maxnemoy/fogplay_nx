#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/media-workers
ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=size=1280x720:rate=30 -frames:v 12 -c:v libx264 -preset ultrafast -tune zerolatency -f h264 -y work/media-workers/frames.h264
audio_flags=()
if [[ "${MEDIA_CPU_POOL:-0}" == 1 ]]; then audio_flags+=(-DFOGPLAY_MEDIA_CPU_POOL=1); fi
if [[ "${BOUNDED_SYNC_AUDIO:-0}" == 1 ]]; then audio_flags+=(-DFOGPLAY_BOUNDED_SYNC_AUDIO); fi
if [[ -n "${STAGE1_FEATURE_MASK:-}" ]]; then
    if [[ ! "$STAGE1_FEATURE_MASK" =~ ^[0-9]{1,3}$ ]] || (( 10#$STAGE1_FEATURE_MASK > 511 )); then
        printf 'STAGE1_FEATURE_MASK must be an integer from 0 to 511.\n' >&2; exit 2
    fi
    audio_flags+=("-DFOGPLAY_STAGE1_FEATURE_MASK=$((10#$STAGE1_FEATURE_MASK))")
fi
g++ "${audio_flags[@]}" -std=c++17 -O2 -Wall -Wextra -Werror -pthread -Isrc \
    $(pkg-config --cflags sdl2 libavcodec libavutil libswscale opus) \
    src/features/streaming/infrastructure/media.cpp tests/media_worker_tests.cpp \
    $(pkg-config --libs sdl2 libavcodec libavutil libswscale opus) \
    -Wl,--wrap=avcodec_open2,--wrap=avcodec_send_packet,--wrap=sws_scale,--wrap=SDL_UpdateYUVTexture,--wrap=opus_decode,--wrap=SDL_QueueAudio,--wrap=SDL_OpenAudioDevice,--wrap=SDL_PauseAudioDevice,--wrap=snprintf,--wrap=__snprintf_chk,--wrap=SDL_GetQueuedAudioSize,--wrap=av_get_pix_fmt_name \
    -o work/media-workers/media_worker_tests
timeout 30s ./work/media-workers/media_worker_tests work/media-workers/frames.h264 2>&1 | tee work/media-workers/tests.log
