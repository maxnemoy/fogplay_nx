#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -Wall -Wextra -Werror -pthread -Isrc -Ithird_party \
    src/features/session/infrastructure/api.cpp src/core/network/http.cpp \
    src/features/auth/infrastructure/auth_storage.cpp src/core/storage/storage.cpp \
    src/features/catalog/infrastructure/mts_catalog_mapper.cpp tests/auth_repository_tests.cpp \
    -lcurl -o work/auth_repository_tests
./work/auth_repository_tests | tee work/logs/auth-repository-tests.log
