#!/usr/bin/env python3
"""Record source pins, local transforms, toolchain and dependency output hashes."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
PREFIX = Path(sys.argv[1])
SOURCE = ROOT / "third_party/moonlight/moonlight-switch"


def command(*args):
    return subprocess.check_output(args, text=True).strip()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


manifest = {
    "platform": "NintendoSwitch",
    "sources": {
        name: command("git", "-C", str(SOURCE / subdir), "rev-parse", "HEAD")
        for name, subdir in {
            "https://github.com/XITRIX/Moonlight-Switch": ".",
            "https://github.com/XITRIX/moonlight-common-c": "extern/moonlight-common-c",
            "https://github.com/cgutman/enet": "extern/moonlight-common-c/enet",
            "https://github.com/sleepybishop/nanors": "extern/moonlight-common-c/nanors",
        }.items()
    },
    "toolchain": command("/opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc", "--version").splitlines()[0],
    "tools": {
        str(path.relative_to(ROOT)): digest(path)
        for path in sorted((ROOT / "tools/moonlight").rglob("*"))
        if path.is_file() and "__pycache__" not in path.parts
    },
    "libraries": {path.name: digest(path) for path in sorted((PREFIX / "lib").glob("*.a"))},
    "diagnostic_headers": {
        "FogMoonlightDiagnostics.h": digest(PREFIX / "include/FogMoonlightDiagnostics.h"),
    },
    "curl": json.loads((PREFIX / "curl-build.json").read_text()),
    "portlibs": {
        name: digest(Path("/opt/devkitpro/portlibs/switch/lib") / name)
        for name in ("libexpat.a", "libz.a")
    },
    "shared_mbedtls": {
        name: digest(ROOT / "third_party/rtc/switch/lib" / name)
        for name in ("libmbedtls.a", "libmbedx509.a", "libmbedcrypto.a", "libeverest.a", "libp256m.a")
    },
    "shared_mbedtls_config": digest(ROOT / "tools/rtc/mbedtls-user-config.h"),
    "validation": "AArch64 Switch standalone and dual-curl full links against exact app RTC mbedTLS ABI; both curl entry points, libnx/mbedTLS backends and app cookies present; no undefined symbols. No device run.",
}
(PREFIX / "build-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
