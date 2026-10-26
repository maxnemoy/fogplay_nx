#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
if ! pkg-config --exists libxml-2.0; then
    echo 'FogHub parser tests require libxml2 development headers (libxml2-dev).' >&2
    exit 1
fi
read -r -a xml_flags <<< "$(pkg-config --cflags --libs libxml-2.0)"
g++ -std=c++17 -Wall -Wextra -Werror -Isrc \
    src/features/providers/infrastructure/foghub_html.cpp tests/foghub_html_tests.cpp \
    "${xml_flags[@]}" -o work/foghub_html_tests
./work/foghub_html_tests | tee work/logs/foghub-html-tests.log
