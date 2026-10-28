#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -Wall -Wextra -Werror -pthread -Isrc -Ithird_party $(pkg-config --cflags libxml-2.0 libcurl) \
    src/features/providers/infrastructure/foghub_{repositories,html,review_html,transport,rental_repository}.cpp \
    src/features/auth/infrastructure/foghub_cookie_storage.cpp src/core/storage/storage.cpp \
    src/features/session/application/paired_checkout.cpp \
    tests/foghub_rental_tests.cpp $(pkg-config --libs libxml-2.0 libcurl) -o work/foghub_rental_tests
timeout 30 ./work/foghub_rental_tests | tee work/logs/foghub-rental-tests.log
g++ -std=c++17 -Wall -Wextra -Werror -pthread -Isrc $(pkg-config --cflags libcurl) \
    src/features/providers/infrastructure/foghub_transport.cpp tests/foghub_rental_transport_tests.cpp \
    -Wl,--wrap=curl_easy_setopt,--wrap=curl_easy_perform \
    $(pkg-config --libs libcurl) -o work/foghub_rental_transport_tests
timeout 30 ./work/foghub_rental_transport_tests | tee work/logs/foghub-rental-transport-tests.log
