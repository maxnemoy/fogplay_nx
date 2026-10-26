#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Ithird_party \
    src/features/catalog/infrastructure/mts_catalog_mapper.cpp tests/catalog_mapper_tests.cpp -o work/catalog_mapper_tests
./work/catalog_mapper_tests | tee work/logs/catalog-mapper-tests.log
