#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
if ! command -v python3 >/dev/null; then
    printf 'libxml2 compatibility requires python3; see docs/libxml2-compat.md\n' >&2
    exit 1
fi
exec python3 tools/libxml2-compat/compat.py run "$@"