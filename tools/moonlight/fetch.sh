#!/usr/bin/env bash
set -Eeuo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
python3 -B "$ROOT/tools/moonlight/fetch.py"
