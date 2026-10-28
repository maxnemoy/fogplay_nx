#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Ithird_party \
    src/core/storage/storage.cpp src/features/settings/infrastructure/rumble_settings_storage.cpp \
    tests/rumble_settings_tests.cpp tests/no_replace_rename.cpp -Wl,--wrap=rename -o work/rumble_settings_tests
./work/rumble_settings_tests | tee work/logs/rumble-settings-tests.log
