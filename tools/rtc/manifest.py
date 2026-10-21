#!/usr/bin/env python3
"""Validate the RTC cache against recipes, patched sources, tools and outputs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
LIBRARIES = ('datachannel', 'juice', 'usrsctp', 'srtp2', 'mbedtls', 'mbedx509',
             'mbedcrypto', 'everest', 'p256m')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(*args):
    return subprocess.check_output(args, stderr=subprocess.PIPE).decode().strip()


def source_inputs(directory):
    """Include nested submodules and local patches, without build/ignored files."""
    result = {str(directory.relative_to(ROOT)) + '/HEAD':
              command('git', '-C', str(directory), 'rev-parse', 'HEAD')}
    paths = subprocess.check_output(['git', '-C', str(directory), 'ls-files',
                                     '--cached', '--others', '--exclude-standard', '-z'])
    for name in sorted(set(paths.decode().split('\0')) - {''}):
        path = directory / name
        if path.is_dir():
            result.update(source_inputs(path))
        else:
            result[path.relative_to(ROOT).as_posix()] = digest(path)
    return result


def inputs(platform):
    result = {path.relative_to(ROOT).as_posix(): digest(path)
              for path in sorted((ROOT / 'tools/rtc').rglob('*'))
              if path.is_file() and '__pycache__' not in path.parts
              and path.suffix in ('.py', '.sh', '.txt', '.h', '.c', '.cpp', '.patch')}
    for name in ('libdatachannel', 'mbedtls'):
        result.update(source_inputs(ROOT / 'third_party/rtc' / name))
    compiler = os.environ.get('CXX', 'c++')
    if platform == 'switch':
        devkit = Path(os.environ.get('DEVKITPRO', '/opt/devkitpro'))
        compiler = str(devkit / 'devkitA64/bin/aarch64-none-elf-g++')
        result['toolchain/Switch.cmake'] = digest(devkit / 'cmake/Switch.cmake')
    result['compiler'] = command(compiler, '--version')
    result['cmake'] = command('cmake', '--version')
    result['environment'] = {name: os.environ.get(name, '') for name in
                             ('CC', 'CXX', 'CFLAGS', 'CXXFLAGS', 'LDFLAGS', 'DEVKITPRO')}
    return result


def outputs(prefix):
    for name in LIBRARIES:
        if not (prefix / 'lib' / ('lib' + name + '.a')).is_file():
            raise ValueError('missing RTC archive: lib' + name + '.a')
    for name in ('rtc/rtc.hpp', 'mbedtls/ssl.h', 'psa/crypto.h', 'mbedtls-user-config.h'):
        if not (prefix / 'include' / name).is_file():
            raise ValueError('missing RTC header: ' + name)
    return {path.relative_to(prefix).as_posix(): digest(path)
            for directory in ('include', 'lib')
            for path in sorted((prefix / directory).rglob('*')) if path.is_file()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('check', 'write'))
    parser.add_argument('platform', choices=('host', 'switch'))
    args = parser.parse_args()
    prefix = ROOT / 'third_party/rtc' / args.platform
    manifest = prefix / 'build-manifest.json'
    try:
        current = {'schema': 1, 'platform': args.platform,
                   'inputs': inputs(args.platform), 'outputs': outputs(prefix)}
        if args.mode == 'check':
            if json.loads(manifest.read_text()) != current:
                raise ValueError('RTC build inputs or installed outputs changed')
        else:
            temporary = manifest.with_suffix('.tmp')
            temporary.write_text(json.dumps(current, sort_keys=True, indent=2) + '\n')
            temporary.replace(manifest)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'RTC {args.platform} cache requires rebuild: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
