#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc -Ithird_party src/features/streaming/application/remote_input.cpp tests/ps_native_input_tests.cpp -o work/ps_native_input_tests
./work/ps_native_input_tests | tee work/logs/ps-native-input-tests.log
