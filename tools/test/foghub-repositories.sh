#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -Wall -Wextra -Werror -pthread -Isrc -Ithird_party $(pkg-config --cflags libxml-2.0 libcurl) \
    src/features/providers/infrastructure/foghub_{repositories,html,transport}.cpp \
    src/features/auth/infrastructure/foghub_cookie_storage.cpp src/core/storage/storage.cpp \
    tests/foghub_repository_tests.cpp $(pkg-config --libs libxml-2.0 libcurl) -o work/foghub_repository_tests
./work/foghub_repository_tests | tee work/logs/foghub-repository-tests.log
