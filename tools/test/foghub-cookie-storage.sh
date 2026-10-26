#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Ithird_party \
    src/features/auth/infrastructure/foghub_cookie_storage.cpp src/core/storage/storage.cpp \
    tests/foghub_cookie_storage_tests.cpp -o work/foghub_cookie_storage_tests
timeout 30 ./work/foghub_cookie_storage_tests | tee work/logs/foghub-cookie-storage-tests.log
