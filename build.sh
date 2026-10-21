#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITA64="${DEVKITA64:-$DEVKITPRO/devkitA64}"
export PATH="$DEVKITA64/bin:$DEVKITPRO/tools/bin:$PATH"
for command in aarch64-none-elf-g++ elf2nro nacptool make pkg-config; do
    if ! command -v "$command" >/dev/null 2>&1; then
        printf 'Missing tool: %s\n' "$command" >&2
        exit 1
    fi
done
if [[ ! -f romfs/cacert.pem ]]; then
    printf 'Missing romfs/cacert.pem (trusted CA bundle).\n' >&2
    exit 1
fi
mkdir -p work/logs dist
if ! python3 tools/rtc/manifest.py check switch; then
    bash tools/rtc/build.sh switch
fi
bash tools/ffmpeg/build-diag-overlay.sh
make -j"${JOBS:-$(nproc)}" "$@" 2>&1 | tee work/logs/switch-build.log
sha256sum dist/fogplay_nx.nro | tee dist/fogplay_nx.nro.sha256
