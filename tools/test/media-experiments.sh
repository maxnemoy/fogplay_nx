#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
output=work/media-experiments
if [[ "${SANITIZE:-0}" == 1 ]]; then output+=-asan; fi
mkdir -p "$output" work/logs
ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=size=640x360:rate=30 -frames:v 1 -c:v libx264 -preset ultrafast -tune zerolatency -f h264 -y "$output/frame.h264"
ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=size=640x360:rate=30 -frames:v 1 -c:v libx265 -tune zerolatency -x265-params pools=1:frame-threads=1:bframes=0:log-level=error -preset ultrafast -f hevc -y "$output/frame.hevc"
flags=(-std=c++17 -O2 -Wall -Wextra -Werror -pthread -Isrc)
if [[ "${SANITIZE:-0}" == 1 ]]; then flags+=(-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer); fi
read -r -a variants <<< "${MEDIA_TEST_VARIANTS:-control nv12 pool textures latest combined old-sdl}"
for variant in "${variants[@]}"; do
    options=(-DFOGPLAY_MEDIA_NV12=0 -DFOGPLAY_MEDIA_CPU_POOL=0 -DFOGPLAY_MEDIA_TEXTURES=1 -DFOGPLAY_MEDIA_READY_DEPTH=3)
    case "$variant" in
        control) ;;
        nv12) options[0]=-DFOGPLAY_MEDIA_NV12=1 ;;
        pool) options[1]=-DFOGPLAY_MEDIA_CPU_POOL=1 ;;
        textures) options[2]=-DFOGPLAY_MEDIA_TEXTURES=2 ;;
        latest) options[3]=-DFOGPLAY_MEDIA_READY_DEPTH=1 ;;
        combined|old-sdl) options=(-DFOGPLAY_MEDIA_NV12=1 -DFOGPLAY_MEDIA_CPU_POOL=1 -DFOGPLAY_MEDIA_TEXTURES=2 -DFOGPLAY_MEDIA_READY_DEPTH=1)
            if [[ "$variant" == old-sdl ]]; then options+=(-include tests/media_old_sdl_api.hpp); fi ;;
        *) printf 'Invalid media test variant: %s\n' "$variant" >&2; exit 2 ;;
    esac
    g++ "${flags[@]}" "${options[@]}" $(pkg-config --cflags sdl2 libavcodec libavutil libswscale opus) \
        src/features/streaming/infrastructure/media.cpp tests/media_experiment_tests.cpp \
        $(pkg-config --libs sdl2 libavcodec libavutil libswscale opus) \
        -Wl,--wrap=avcodec_receive_frame,--wrap=av_hwframe_transfer_data,--wrap=SDL_CreateTexture,--wrap=SDL_DestroyTexture,--wrap=SDL_UpdateYUVTexture,--wrap=SDL_UpdateNVTexture,--wrap=SDL_RenderCopy \
        -o "$output/$variant"
    "$output/$variant" "$output/frame.h264"
    "$output/$variant" "$output/frame.hevc" hevc
done
