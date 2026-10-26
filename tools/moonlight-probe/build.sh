#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITA64="${DEVKITA64:-$DEVKITPRO/devkitA64}"
export PATH="$DEVKITA64/bin:$DEVKITPRO/tools/bin:$PATH"
bash tools/moonlight/build.sh
if [[ ! -f third_party/ffmpeg-diag/switch/lib/libavcodec.a ]]; then
    python3 tools/ffmpeg/build-diag-overlay.py
fi
make -C tools/moonlight-probe -j"${JOBS:-4}"
