#!/usr/bin/env python3
"""Check the cached production dependency recipe and exact archive hashes."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[2]
prefix = root / "third_party/moonlight/switch"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


try:
    manifest = json.loads((prefix / "build-manifest.json").read_text())
    current = {str(path.relative_to(root)): digest(path)
               for path in sorted((root / "tools/moonlight").rglob("*"))
               if path.is_file() and "__pycache__" not in path.parts}
    assert manifest["tools"] == current
    for name, expected in manifest["libraries"].items():
        assert digest(prefix / "lib" / name) == expected
    for name in ("libgamestream.a", "libmoonlight-common-c.a", "libmoonlight-curl.a", "libenet.a"):
        assert name in manifest["libraries"]
    for name, expected in manifest["portlibs"].items():
        assert digest(Path("/opt/devkitpro/portlibs/switch/lib") / name) == expected
    for name, expected in manifest["shared_mbedtls"].items():
        assert digest(root / "third_party/rtc/switch/lib" / name) == expected
    assert digest(root / "tools/rtc/mbedtls-user-config.h") == manifest["shared_mbedtls_config"]
except (OSError, KeyError, ValueError, AssertionError):
    raise SystemExit(1)
