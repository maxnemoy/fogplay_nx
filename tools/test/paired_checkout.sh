#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -Wall -Wextra -Werror -pthread -Isrc \
    src/features/session/application/paired_checkout.cpp tests/paired_checkout_tests.cpp \
    -o work/paired_checkout_tests
./work/paired_checkout_tests | tee work/logs/paired-checkout-tests.log
