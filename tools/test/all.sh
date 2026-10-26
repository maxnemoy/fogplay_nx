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
for suite in build-tools host repositories foghub-transport foghub-cookie-storage foghub-html foghub-repositories catalog-domain stream-input ps-sensors ps-native-input playstation-catalog artwork steam-artwork catalog-artwork recent-games control-sender ice-snapshot stream timed-mutex loop-timing playback-drain media h264-rtp rtp-repair rtp-media hevc media-presentation decoder-diagnostics previews; do
    printf '\nRunning %s tests\n' "$suite"
    bash "tools/test/$suite.sh"
done
if (( extended_rtc )); then bash tools/test/rtc.sh; fi
if (( interop )); then
    if [[ ! -x work/av-test/venv/bin/python ]]; then
        printf 'Interop needs the pre-provisioned work/av-test/venv; see tools/av-test/pc/requirements.txt.\n' >&2
        exit 1
    fi
    bash tools/av-test/tests/run-interop.sh
fi
