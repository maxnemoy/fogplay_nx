#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
mkdir -p work/logs
g++ -std=c++17 -Wall -Wextra -Werror -pthread -Isrc -Ithird_party \
    src/features/session/infrastructure/api.cpp src/core/network/http.cpp src/features/auth/infrastructure/auth_storage.cpp src/core/storage/storage.cpp tests/no_replace_rename.cpp tests/api_tests.cpp -Wl,--wrap=rename -lcurl -o work/api_tests
./work/api_tests | tee work/logs/api-tests.log
g++ -std=c++17 -Wall -Wextra -Werror -pthread -Isrc -Ithird_party \
    src/features/session/infrastructure/api.cpp src/core/network/http.cpp src/features/auth/infrastructure/auth_storage.cpp src/core/storage/storage.cpp src/features/session/application/session.cpp tests/no_replace_rename.cpp tests/session_tests.cpp -Wl,--wrap=rename -lcurl -o work/session_tests
./work/session_tests | tee work/logs/session-tests.log
g++ -std=c++17 -Wall -Wextra -Werror -pthread -Isrc -Ithird_party \
    src/features/session/infrastructure/api.cpp src/core/network/http.cpp src/features/auth/infrastructure/auth_storage.cpp src/core/storage/storage.cpp src/features/session/application/session.cpp src/features/session/application/checkout.cpp tests/no_replace_rename.cpp tests/checkout_tests.cpp -Wl,--wrap=rename -lcurl -o work/checkout_tests
./work/checkout_tests | tee work/logs/checkout-tests.log
g++ -std=c++17 -Wall -Wextra -Werror -pthread -Isrc -Ithird_party \
    src/features/session/infrastructure/api.cpp src/core/network/http.cpp src/features/auth/infrastructure/auth_storage.cpp src/core/storage/storage.cpp tests/no_replace_rename.cpp tests/auth_storage_tests.cpp -Wl,--wrap=rename -lcurl -o work/auth_storage_tests
./work/auth_storage_tests | tee work/logs/auth-storage-tests.log
g++ -std=c++17 -Wall -Wextra -Werror -Isrc \
    src/core/storage/storage.cpp tests/storage_tests.cpp -Wl,--wrap=rename,--wrap=unlink,--wrap=fsync -o work/storage_tests
./work/storage_tests | tee work/logs/storage-tests.log
g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Ithird_party \
    tests/catalog_tests.cpp -o work/catalog_tests
./work/catalog_tests | tee work/logs/catalog-tests.log
g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Ithird_party \
    src/features/streaming/application/remote_input.cpp tests/remote_input_tests.cpp -o work/remote_input_tests
./work/remote_input_tests | tee work/logs/remote-input-tests.log
g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Ithird_party $(pkg-config --cflags sdl2 SDL2_ttf SDL2_image) \
    src/features/streaming/presentation/stream_overlay.cpp src/app/presentation/ui.cpp tests/stream_overlay_tests.cpp \
    $(pkg-config --libs sdl2 SDL2_ttf SDL2_image) -o work/stream_overlay_tests
./work/stream_overlay_tests | tee work/logs/stream-overlay-tests.log
g++ -std=c++17 -Wall -Wextra -Werror -Isrc tests/video_recovery_tests.cpp -o work/video_recovery_tests
./work/video_recovery_tests | tee work/logs/video-recovery-tests.log
g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread -Isrc src/core/logging/run_log.cpp tests/run_log_tests.cpp -Wl,--wrap=write -Wl,--wrap=fsync -o work/run_log_tests
./work/run_log_tests | tee work/logs/run-log-tests.log
g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Ithird_party \
    src/core/storage/storage.cpp src/features/settings/infrastructure/video_settings_storage.cpp tests/video_settings_tests.cpp tests/no_replace_rename.cpp \
    -Wl,--wrap=rename -o work/video_settings_tests
./work/video_settings_tests | tee work/logs/video-settings-tests.log
g++ -std=c++17 -Wall -Wextra -Werror -pthread -Isrc -Ithird_party \
    src/features/catalog/infrastructure/latency.cpp src/core/network/http.cpp tests/latency_tests.cpp -lcurl -o work/latency_tests
./work/latency_tests | tee work/logs/latency-tests.log
if [[ "${1:-}" == "--live" ]]; then
    g++ -std=c++17 -Wall -Wextra -Werror -pthread -Isrc -Ithird_party \
        src/features/session/infrastructure/api.cpp src/core/network/http.cpp src/features/auth/infrastructure/auth_storage.cpp src/core/storage/storage.cpp tests/live_probe.cpp -lcurl -o work/live_probe
    ./work/live_probe | tee work/logs/live-probe.log
fi

bash tools/test/controller-layout.sh

g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Ithird_party \
    src/features/session/application/session.cpp src/features/session/application/checkout.cpp \
    src/core/storage/storage.cpp tests/rental_service_tests.cpp -o work/rental_service_tests
./work/rental_service_tests | tee work/logs/rental-service-tests.log
