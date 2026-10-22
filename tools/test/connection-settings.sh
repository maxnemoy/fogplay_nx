#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Ithird_party \
    src/core/storage/storage.cpp src/features/settings/infrastructure/connection_settings_storage.cpp \
    tests/connection_settings_tests.cpp tests/no_replace_rename.cpp -Wl,--wrap=rename -o work/connection_settings_tests
./work/connection_settings_tests | tee work/logs/connection-settings-tests.log
