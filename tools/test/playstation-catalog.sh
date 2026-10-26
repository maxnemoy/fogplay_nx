#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Ithird_party \
    src/features/catalog/infrastructure/mts_catalog_mapper.cpp src/features/playstation/infrastructure/catalog.cpp src/features/playstation/infrastructure/availability.cpp src/core/network/http.cpp \
    tests/playstation_catalog_tests.cpp -lcurl -o work/playstation_catalog_tests
./work/playstation_catalog_tests "$@" | tee work/logs/playstation-catalog-tests.log

g++ -std=c++17 -Wall -Wextra -Werror -pthread -Isrc -Ithird_party \
    src/features/catalog/infrastructure/mts_catalog_mapper.cpp src/features/playstation/infrastructure/catalog.cpp src/features/playstation/infrastructure/availability.cpp src/core/network/http.cpp \
    tests/playstation_progress_tests.cpp -lcurl -o work/playstation_progress_tests
./work/playstation_progress_tests | tee work/logs/playstation-progress-tests.log

python3 tests/playstation_checkout_tests.py | tee work/logs/playstation-checkout-tests.log

python3 tests/playstation_progress_glue_tests.py | tee work/logs/playstation-progress-glue-tests.log
