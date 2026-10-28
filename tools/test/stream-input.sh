#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Ithird_party \
    tests/stream_input_gate_tests.cpp -o work/stream_input_gate_tests
./work/stream_input_gate_tests | tee work/logs/stream-input-gate-tests.log

g++ -std=c++17 -Wall -Wextra -Werror -Isrc tests/playstation_controller_tests.cpp -o work/playstation_controller_tests
./work/playstation_controller_tests | tee work/logs/playstation-controller-tests.log

g++ -std=c++17 -Wall -Wextra -Werror -Isrc tests/mouse_buttons_tests.cpp -o work/mouse_buttons_tests
./work/mouse_buttons_tests | tee work/logs/mouse-buttons-tests.log
