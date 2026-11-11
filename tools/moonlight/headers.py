#!/usr/bin/env python3
"""Publish only the closed, generated Moonlight include tree (26 headers)."""
import argparse
import fcntl
import os
import stat
from pathlib import Path
import tarfile

import prepare

PREFIX = prepare.BASE / 'switch'
TARGET = PREFIX / 'include'
STAGING = PREFIX / '.include.staging'
BACKUP = PREFIX / '.include.backup'
CURL_ARCHIVE = Path('work/moonlight/downloads/curl-8.15.0.tar.xz')
CURL_SHA256 = '6cd0a8a5b126ddfda61c94dc2c3fc53481ba7a35461cf7c5ab66aa9d6775b609'
CURL_NAMES = ('curl.h', 'curlver.h', 'easy.h', 'header.h', 'mprintf.h', 'multi.h',
              'options.h', 'stdcheaders.h', 'system.h', 'typecheck-gcc.h', 'urlapi.h', 'websockets.h')


def header_sources():
    result = {'gamestream/' + name: prepare.PREPARED / 'libgamestream' / name
              for name in ('client.h', 'errors.h', 'http.h', 'xml.h')}
    result.update({'crypto/' + name: prepare.PREPARED / 'crypto' / name for name in
                   ('CryptoManager.hpp', 'Data.hpp', 'MbedTLSCryptoManager.hpp', 'OpenSSLCryptoManager.hpp')})
    result.update({name: Path('tools/moonlight') / name for name in
                   ('FogMoonlightDiagnostics.h', 'FogMoonlightRecovery.h', 'FogMoonlightTermination.h')})
    result.update({name: Path('tools/moonlight/compat') / name for name in ('Settings.hpp', 'Diagnostics.hpp')})
    result['Limelight.h'] = prepare.BASE / 'moonlight-switch/extern/moonlight-common-c/src/Limelight.h'
    return result


def expected_bytes(root):
    result = {}
    for name, source in header_sources().items():
        prepare.validate_ancestry(root, source, allow_missing=False)
        if not stat.S_ISREG((root / source).lstat().st_mode):
            raise prepare.PrepareError('non-regular exported header source: ' + str(source))
        result[name] = (root / source).read_bytes()
    prepare.validate_ancestry(root, CURL_ARCHIVE, allow_missing=False)
    if prepare.digest(root / CURL_ARCHIVE) != CURL_SHA256:
        raise prepare.PrepareError('private curl archive checksum mismatch')
    with tarfile.open(root / CURL_ARCHIVE) as archive:
        for name in CURL_NAMES:
            member = archive.getmember('curl-8.15.0/include/curl/' + name)
            if not member.isfile():
                raise prepare.PrepareError('non-regular curl header: ' + name)
            result['curl/' + name] = archive.extractfile(member).read()
    return result


def validate_existing(root):
    for relative in (PREFIX, TARGET, STAGING, BACKUP):
        prepare.validate_ancestry(root, relative)
    for relative in (STAGING, BACKUP):
        if prepare.lexists(root / relative):
            raise prepare.PrepareError(f'incomplete header publication; inspect {root / STAGING}, '
                                       f'{root / BACKUP} and {root / TARGET}; automatic recovery disabled')
    if not prepare.lexists(root / TARGET):
        return 'absent'
    files, directories = prepare.scan_tree(root / TARGET)
    names = set(header_sources()) | {'curl/' + name for name in CURL_NAMES}
    if not set(files) <= names or not directories <= {'gamestream', 'crypto', 'curl'}:
        raise prepare.PrepareError('unknown installed include paths; refusing to delete or replace them')
    return 'existing'


def installed_digests(root, expected=None):
    validate_existing(root)
    expected = expected if expected is not None else expected_bytes(root)
    actual, directories = prepare.scan_tree(root / TARGET)
    hashes = {name: prepare.digest_bytes(data) for name, data in expected.items()}
    if actual != hashes or directories != {'gamestream', 'crypto', 'curl'}:
        raise prepare.PrepareError('installed include tree differs from the complete exported header set')
    return {'include/' + name: value for name, value in sorted(actual.items())}


def publish(root=prepare.ROOT):
    root = Path(root).absolute()
    validate_existing(root)
    (root / PREFIX).mkdir(parents=True, exist_ok=True)
    lockpath = root / PREFIX / '.include.lock'
    if prepare.lexists(lockpath) and not stat.S_ISREG(lockpath.lstat().st_mode):
        raise prepare.PrepareError('header lock is not a regular file: ' + str(lockpath))
    descriptor = os.open(lockpath, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, 'r+') as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        state = validate_existing(root)
        series = prepare.load_series(root)
        prepare.check_recovery_paths(root)
        prepare.validate_patch_inputs(root, series)
        prepare.validate_source_paths(root, series)
        prepare.validate_repositories(root, series)
        prepare.prepared_digests(root, series)
        expected = expected_bytes(root)
        if state != 'absent':
            actual, directories = prepare.scan_tree(root / TARGET)
            if (actual == {name: prepare.digest_bytes(data) for name, data in expected.items()}
                    and directories == {'gamestream', 'crypto', 'curl'}):
                return 'unchanged'
        stage = root / STAGING
        stage.mkdir()
        try:
            for name, data in expected.items():
                target = stage / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
            files, _ = prepare.scan_tree(stage)
            if files != {name: prepare.digest_bytes(data) for name, data in expected.items()}:
                raise prepare.PrepareError('staged header bytes changed')
        except Exception:
            prepare.remove_owned_tree(stage)
            raise
        def validate_published():
            actual, directories = prepare.scan_tree(root / TARGET)
            if actual != files or directories != {'gamestream', 'crypto', 'curl'}:
                raise prepare.PrepareError('published header bytes changed')
        prepare.publish_staged(root, stage, root / TARGET, root / BACKUP, state, validate_published)
        return 'published'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--preflight', action='store_true', help='reject unknown install paths without publication')
    args = parser.parse_args()
    print(validate_existing(prepare.ROOT) if args.preflight else publish())
