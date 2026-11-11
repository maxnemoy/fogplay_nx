#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
extended_rtc=0
interop=0
for option in "$@"; do
    case "$option" in
        --extended-rtc) extended_rtc=1 ;;
        --interop) interop=1 ;;
        *) printf 'Usage: %s [--extended-rtc] [--interop]\n' "$0" >&2; exit 2 ;;
    esac
done
python3 tools/check-architecture.py
python3 tests/stage1_build_inputs_tests.py
python3 -m unittest discover -s tools/diagnostics -p "test_*.py"
bash tools/test/thread-observation.sh
bash tools/test/transport-teardown.sh
bash tools/test/rental-stop-coordinator.sh
bash tools/test/rental-session-coordinator.sh
bash tools/test/player.sh
bash tools/test/media-signals.sh
bash tools/test/stream-ports.sh
bash tools/test/stage0-metrics.sh
bash tools/test/video-latency.sh
bash tools/test/app-diagnostics.sh
bash tools/test/diagnostic-profile.sh
bash tools/test/telemetry.sh
bash tools/test/telemetry-providers.sh
bash tools/test/telemetry-network.sh
bash tools/test/network-telemetry.sh
bash tools/test/media-telemetry.sh
bash tools/test/run-log-batches.sh
bash tools/test/ui-telemetry.sh
bash tools/test/network-event-batches.sh
bash tools/test/protocol-telemetry.sh
bash tools/test/player-telemetry.sh
bash tools/test/ps-input-telemetry.sh
bash tools/test/rumble.sh
bash tools/test/rumble-settings.sh
bash tools/test/video-watchdog.sh
for suite in deferred-settings build-tools host repositories foghub-transport foghub-cookie-storage foghub-html foghub-rental-html foghub-libxml-compat foghub-repositories foghub-rentals foghub-review session-rating-ui paired_checkout paired-video-settings-controller paired-checkout-storage catalog-domain stream-input ps-sensors ps-native-input playstation-catalog artwork steam-artwork recent-games control-sender ice-snapshot stream timed-mutex loop-timing playback-drain media media-workers media-experiments h264-rtp rtp-repair rtp-media hevc media-presentation decoder-diagnostics previews; do
    printf '\nRunning %s tests\n' "$suite"
    bash "tools/test/$suite.sh"
done
bash tools/test/typed-input.sh
if (( extended_rtc )); then bash tools/test/rtc.sh; fi
if (( interop )); then
    if [[ ! -x work/av-test/venv/bin/python ]]; then
        printf 'Interop needs the pre-provisioned work/av-test/venv; see tools/av-test/pc/requirements.txt.\n' >&2
        exit 1
    fi
    bash tools/av-test/tests/run-interop.sh
fi
