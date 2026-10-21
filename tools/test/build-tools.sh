#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
python3 tools/test/build_tools_tests.py 2>&1 | tee work/logs/build-tools-tests.log
