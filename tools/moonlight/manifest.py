#!/usr/bin/env python3
"""Record and compare exact Moonlight source, tool, and installed-output hashes."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

import prepare as preparation
import headers

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'third_party/moonlight/moonlight-switch'
PORTLIBS = Path('/opt/devkitpro/portlibs/switch/lib')
COMPILER = '/opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc'
SOURCE_URLS = {
    'switch': 'https://github.com/XITRIX/Moonlight-Switch',
    'common-c': 'https://github.com/XITRIX/moonlight-common-c',
    'enet': 'https://github.com/cgutman/enet',
    'nanors': 'https://github.com/sleepybishop/nanors',
}


def command(*args):
    return subprocess.check_output(args, text=True).strip()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build_manifest(prefix):
    prefix = Path(prefix)
    if prefix.absolute() != (ROOT / headers.PREFIX).absolute():
        raise ValueError('manifest prefix must be the private Moonlight install tree')
    series = preparation.load_series(ROOT)
    preparation.validate_patch_inputs(ROOT, series)
    preparation.validate_source_paths(ROOT, series)
    preparation.validate_repositories(ROOT, series)
    prepared = preparation.prepared_digests(ROOT, series)
    preparation.check_recovery_paths(ROOT)
    installed_headers = headers.installed_digests(ROOT)
    libraries = {}
    for name in ('libgamestream.a', 'libmoonlight-common-c.a', 'libmoonlight-curl.a', 'libenet.a'):
        path = prefix / 'lib' / name
        preparation.validate_ancestry(ROOT, path.relative_to(ROOT), allow_missing=False)
        if path.is_symlink() or not path.is_file():
            raise ValueError('missing or non-regular Moonlight archive: ' + name)
        libraries[name] = digest(path)
    return {
        'schema': 2,
        'platform': 'NintendoSwitch',
        'sources': {
            SOURCE_URLS[repository['name']]: preparation.revision(ROOT / repository['path'])
            for repository in series['repositories']
        },
        'toolchain': command(COMPILER, '--version').splitlines()[0],
        'tools': {
            str(path.relative_to(ROOT)): digest(path)
            for path in sorted((ROOT / 'tools/moonlight').rglob('*'))
            if path.is_file() and '__pycache__' not in path.parts
        },
        'prepared': prepared,
        'libraries': libraries,
        'installed_headers': installed_headers,
        'curl': json.loads((prefix / 'curl-build.json').read_text()),
        'curl_symbol_map': digest(prefix / 'curl-symbols.map'),
        'portlibs': {
            name: digest(PORTLIBS / name)
            for name in ('libexpat.a', 'libz.a')
        },
        'shared_mbedtls': {
            name: digest(ROOT / 'third_party/rtc/switch/lib' / name)
            for name in ('libmbedtls.a', 'libmbedx509.a', 'libmbedcrypto.a', 'libeverest.a', 'libp256m.a')
        },
        'shared_mbedtls_config': digest(ROOT / 'tools/rtc/mbedtls-user-config.h'),
        'validation': 'AArch64 Switch standalone and dual-curl full links against exact app RTC mbedTLS ABI; both curl entry points, libnx/mbedTLS backends and app cookies present; no undefined symbols. No device run.',
    }


def main():
    prefix = Path(sys.argv[1])
    manifest = build_manifest(prefix)
    temporary = prefix / '.build-manifest.json.tmp'
    temporary.write_text(json.dumps(manifest, indent=2) + '\n')
    temporary.replace(prefix / 'build-manifest.json')


if __name__ == '__main__':
    main()
