#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -Wall -Wextra -Werror -pthread -Isrc tests/deferred_settings_tests.cpp -o work/deferred_settings_tests
./work/deferred_settings_tests | tee work/logs/deferred-settings-tests.log
