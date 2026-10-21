#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/layout0413
g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Ithird_party \
    src/features/settings/domain/controller_layout.cpp src/features/settings/infrastructure/controller_layout_storage.cpp src/core/storage/storage.cpp tests/controller_layout_tests.cpp \
    -Wl,--wrap=rename,--wrap=fsync,--wrap=read -o work/layout0413/controller_layout_tests
./work/layout0413/controller_layout_tests | tee work/layout0413/controller-layout-tests.log
