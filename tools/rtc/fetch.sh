#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$ROOT/third_party/rtc"
fetch() {
  local name=$1 url=$2 tag=$3 commit=$4
  local target="$ROOT/third_party/rtc/$name"
  if [ ! -d "$target" ]; then
    git -c advice.detachedHead=false clone --depth 1 --branch "$tag" --recurse-submodules --shallow-submodules "$url" "$target"
  fi
  [ "$(git -C "$target" rev-parse HEAD)" = "$commit" ] || { echo "Unexpected $name revision; existing sources preserved" >&2; exit 1; }
  local submodules
  submodules=$(git -C "$target" submodule status --recursive)
  if grep -qE '^[-+U]' <<< "$submodules"; then
    echo "Unexpected $name submodule state" >&2; exit 1
  fi
  return 0
}
fetch libdatachannel https://github.com/paullouisageneau/libdatachannel.git v0.24.1 a02b751917ac8afc8c58dc6f4461d25ff9465d48
fetch mbedtls https://github.com/Mbed-TLS/mbedtls.git mbedtls-3.6.4 c765c831e5c2a0971410692f92f7a81d6ec65ec2
# patch-sources.py is the active patch recipe. The *.patch files are passive
# historical diffs retained for provenance; do not apply them a second time.
python3 "$ROOT/tools/rtc/patch-sources.py"
