#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -Wall -Wextra -Werror -pthread -Isrc $(pkg-config --cflags libcurl) \
    src/features/providers/infrastructure/foghub_transport.cpp tests/foghub_transport_tests.cpp \
    -Wl,--wrap=curl_easy_setopt,--wrap=curl_easy_getinfo,--wrap=curl_easy_perform \
    $(pkg-config --libs libcurl) -o work/foghub_transport_tests
timeout 30 ./work/foghub_transport_tests | tee work/logs/foghub-transport-tests.log
