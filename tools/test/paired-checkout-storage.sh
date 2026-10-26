#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Ithird_party \
    src/core/storage/storage.cpp src/features/session/infrastructure/paired_checkout_storage.cpp \
    tests/paired_checkout_storage_tests.cpp -o work/paired_checkout_storage_tests
./work/paired_checkout_storage_tests | tee work/logs/paired-checkout-storage-tests.log
