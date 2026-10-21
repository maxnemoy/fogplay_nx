#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
source "$DEVKITPRO/switchvars.sh"
export PKG_CONFIG_LIBDIR="$PORTLIBS_PREFIX/lib/pkgconfig"
python3 tools/ffmpeg/build-diag-overlay.py
