#!/usr/bin/env bash
set -Eeuo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BASE="$ROOT/third_party/moonlight"
SOURCE="$BASE/moonlight-switch"
SWITCH_PIN=a75fee223914f36ec26b29739d95331307960fa3
CORE_PIN=206c79c240c342229555494a8cadda6c11cad479
mkdir -p "$BASE"
if [ ! -d "$SOURCE/.git" ]; then
  git clone --no-checkout https://github.com/XITRIX/Moonlight-Switch.git "$SOURCE"
fi
if ! git -C "$SOURCE" cat-file -e "$SWITCH_PIN^{commit}"; then
  git -C "$SOURCE" fetch origin "$SWITCH_PIN"
fi
if [ -n "$(git -C "$SOURCE" status --porcelain --untracked-files=no --ignore-submodules=all)" ]; then
  echo 'Moonlight source contains local changes; refusing checkout' >&2
  exit 1
fi
git -C "$SOURCE" checkout --detach "$SWITCH_PIN"
git -C "$SOURCE" submodule update --init extern/moonlight-common-c
test "$(git -C "$SOURCE/extern/moonlight-common-c" rev-parse HEAD)" = "$CORE_PIN"
git -C "$SOURCE/extern/moonlight-common-c" submodule update --init --recursive
for directory in "$SOURCE/extern/moonlight-common-c" \
                 "$SOURCE/extern/moonlight-common-c/enet" \
                 "$SOURCE/extern/moonlight-common-c/nanors"; do
  if [ -n "$(git -C "$directory" status --porcelain --untracked-files=no --ignore-submodules=all)" ]; then
    echo "Moonlight dependency contains local changes: $directory" >&2
    exit 1
  fi
done
python3 "$ROOT/tools/moonlight/prepare.py"
