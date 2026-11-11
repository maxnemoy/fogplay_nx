#!/usr/bin/env python3
"""Validate and publish the pinned Moonlight prepared-source patch series."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
BASE = Path('third_party/moonlight')
PREPARED = BASE / 'prepared'
STAGING = BASE / '.prepared.staging'
BACKUP = BASE / '.prepared.backup'
SERIES = Path('tools/moonlight/patches/series.json')
HASH = re.compile(r'[0-9a-f]{64}\Z')
REVISION = re.compile(r'[0-9a-f]{40}\Z')


class PrepareError(RuntimeError):
    pass


def digest_bytes(data):
    return hashlib.sha256(data).hexdigest()


def digest(path):
    return digest_bytes(path.read_bytes())


def lexists(path):
    return os.path.lexists(path)


def checked_path(value, label):
    if not isinstance(value, str):
        raise PrepareError(f'invalid {label}')
    path = PurePosixPath(value)
    if (not value or value == '.' or '\\' in value or ':' in value
            or any(ord(char) < 32 for char in value) or path.is_absolute()
            or '..' in path.parts or str(path) != value):
        raise PrepareError(f'unsafe {label}: {value!r}')
    return value


def checked_hash(value, label):
    if not isinstance(value, str) or not HASH.fullmatch(value):
        raise PrepareError(f'invalid {label}')
    return value


def validate_ancestry(root, relative, allow_missing=True):
    # Preserve '..' until ancestry inspection: lexical normalization can hide
    # a symlink component whose filesystem meaning differs from that spelling.
    root = Path(root).absolute()
    for ancestor in reversed((root, *root.parents)):
        if ancestor.is_symlink():
            raise PrepareError('symbolic link in fixture ancestry: ' + str(ancestor))
    path = root
    for part in PurePosixPath(relative).parts:
        path /= part
        try:
            mode = path.lstat().st_mode
        except FileNotFoundError:
            if allow_missing:
                continue
            raise PrepareError('required path is missing: ' + str(path))
        if stat.S_ISLNK(mode):
            raise PrepareError('symbolic link in path: ' + str(path))
        if path != root / relative and not stat.S_ISDIR(mode):
            raise PrepareError('path parent is not a directory: ' + str(path))
    resolved_root = root.resolve(strict=True)
    resolved_parent = path.parent.resolve(strict=False)
    if resolved_parent != resolved_root and resolved_root not in resolved_parent.parents:
        raise PrepareError('path escapes fixture root: ' + str(path))


def load_series(root):
    validate_ancestry(root, SERIES)
    try:
        if not stat.S_ISREG((root / SERIES).lstat().st_mode):
            raise PrepareError('series metadata is not a regular file')
        data = json.loads((root / SERIES).read_text())
        if data.get('schema') != 2:
            raise PrepareError('unrecognized Moonlight series schema')
        repositories = data['repositories']
        patches = data['patches']
        outputs = data['outputs']
        if not isinstance(repositories, list) or not isinstance(patches, list) or not isinstance(outputs, list):
            raise PrepareError('invalid Moonlight series collections')
        if len(outputs) != 24 or len(patches) != 10:
            raise PrepareError('Moonlight series must declare 24 outputs and ten ordered patches')
        repository_names = set()
        repository_paths = set()
        for repository in repositories:
            name = repository['name']
            if not isinstance(name, str) or not name or name in repository_names:
                raise PrepareError('invalid or duplicate repository name')
            path = checked_path(repository['path'], 'repository path')
            if path in repository_paths:
                raise PrepareError('duplicate repository path: ' + path)
            if not isinstance(repository['revision'], str) or not REVISION.fullmatch(repository['revision']):
                raise PrepareError('invalid repository revision')
            repository_names.add(name)
            repository_paths.add(path)
        if repository_names != {'switch', 'common-c', 'enet', 'nanors'}:
            raise PrepareError('Moonlight series must pin switch, common-c, enet and nanors')
        patch_names = set()
        for index, patch in enumerate(patches):
            name = checked_path(patch['path'], 'patch path')
            if Path(name).name != name or name in patch_names:
                raise PrepareError('invalid or duplicate patch: ' + name)
            checked_hash(patch['sha256'], 'patch SHA-256')
            if index < 8:
                checked_path(patch['legacy_source'], 'legacy provenance path')
                checked_hash(patch['legacy_sha256'], 'legacy provenance SHA-256')
            elif patch.get('provenance') != (
                    'post-frozen same-wait IDR recovery correction' if index == 8
                    else 'restore per-loss IDR retry after 0.6.33 regression'):
                raise PrepareError('missing post-frozen patch provenance')
            if not isinstance(patch['transformation'], str) or not patch['transformation']:
                raise PrepareError('missing patch transformation provenance')
            patch_names.add(name)
        output_names = set()
        for output in outputs:
            name = checked_path(output['path'], 'prepared path')
            if name in output_names:
                raise PrepareError('duplicate prepared output: ' + name)
            if output['repository'] not in repository_names:
                raise PrepareError('unknown output repository: ' + output['repository'])
            checked_path(output['source_path'], 'source path')
            checked_hash(output['base_sha256'], 'base SHA-256')
            checked_hash(output['prepared_sha256'], 'prepared SHA-256')
            checked_hash(output['frozen_sha256'], 'frozen C7 SHA-256')
            checked_hash(output['previous_sha256'], '0.6.33 SHA-256')
            output_names.add(name)
        patch_root = root / SERIES.parent
        actual = set()
        with os.scandir(patch_root) as entries:
            for entry in entries:
                if entry.is_symlink() or not entry.is_file(follow_symlinks=False):
                    raise PrepareError('non-regular path in patch directory: ' + str(Path(entry.path)))
                actual.add(entry.name)
        if actual != patch_names | {'series.json'}:
            raise PrepareError('patch directory and series metadata disagree')
        if set(data['tool_inputs']) != {'tools/moonlight/prepare.py'}:
            raise PrepareError('unexpected active preparation tool inventory')
        for name, value in data['tool_inputs'].items():
            checked_path(name, 'active tool path')
            checked_hash(value, 'active tool SHA-256')
        return data
    except (OSError, KeyError, TypeError, json.JSONDecodeError) as error:
        if isinstance(error, PrepareError):
            raise
        raise PrepareError(f'cannot load Moonlight series: {error}') from error


def repository_map(series):
    return {repository['name']: repository for repository in series['repositories']}


def git_environment():
    # Never inherit an index/worktree/config redirect from a caller's shell.
    return {key: value for key, value in os.environ.items()
            if not key.startswith('GIT_')}


def revision(repository):
    return subprocess.check_output(
        ['git', '-C', str(repository), 'rev-parse', 'HEAD'],
        stderr=subprocess.PIPE, text=True, env=git_environment()).strip()


def git_file(repository, commit, path):
    return subprocess.check_output(
        ['git', '-C', str(repository), 'show', f'{commit}:{path}'],
        stderr=subprocess.PIPE, env=git_environment())


def command_text(*args):
    return subprocess.check_output(args, stderr=subprocess.PIPE, text=True,
                                   env=git_environment()).strip()


def validate_source_paths(root, series):
    for repository in series['repositories']:
        relative = PurePosixPath(repository['path'])
        validate_ancestry(root, relative, allow_missing=False)
        source = root / relative
        if not source.is_dir():
            raise PrepareError('source repository is not a directory: ' + str(source))
        actual = Path(command_text('git', '-C', str(source), 'rev-parse', '--show-toplevel'))
        if actual.resolve(strict=True) != source.resolve(strict=True):
            raise PrepareError(
                f'Git worktree identity mismatch: expected {source.resolve()}, got {actual.resolve()}')
        validate_ancestry(root, SERIES.parent)
    repositories = repository_map(series)
    for output in series['outputs']:
        repository = repositories[output['repository']]
        relative = PurePosixPath(repository['path']) / PurePosixPath(output['source_path'])
        validate_ancestry(root, relative)


def validate_index_flags(source):
    # git diff trusts these index hints and can hide edits in compiled units
    # outside the prepared inventory (for example PlatformCrypto.c).
    entries = subprocess.check_output([
        'git', '-C', str(source), 'ls-files', '-v', '-z'],
        stderr=subprocess.PIPE, env=git_environment())
    hidden = [entry[2:].decode(errors='replace') for entry in entries.split(b'\0')
              if entry and (entry[:1] == b'S' or entry[:1].islower())]
    if hidden:
        raise PrepareError(
            f'source index has assume-unchanged/skip-worktree entries in {source}; '
            'refusing hidden local edits without changing flags: ' + ', '.join(hidden))


def validate_repositories(root, series):
    for repository in series['repositories']:
        source = root / repository['path']
        actual = revision(source)
        if actual != repository['revision']:
            raise PrepareError(
                f'revision mismatch for {repository["name"]}: '
                f'expected {repository["revision"]}, got {actual}')
        validate_index_flags(source)
        tracked = subprocess.check_output([
            'git', '-C', str(source), 'diff', '--name-only', 'HEAD', '-z',
            '--ignore-submodules=none'], stderr=subprocess.PIPE, env=git_environment())
        untracked = subprocess.check_output([
            'git', '-C', str(source), 'ls-files', '--others', '-z'],
            stderr=subprocess.PIPE, env=git_environment())
        changes = set(tracked.decode().split('\0')) | set(untracked.decode().split('\0'))
        changes.discard('')
        if changes:
            raise PrepareError(
                f'local source edits in {repository["name"]}: ' + ', '.join(sorted(changes)))


def validate_patch_inputs(root, series):
    for name, expected in series['tool_inputs'].items():
        validate_ancestry(root, name, allow_missing=False)
        if not stat.S_ISREG((root / name).lstat().st_mode):
            raise PrepareError('active tool is not a regular file: ' + name)
        if digest(root / name) != expected:
            raise PrepareError('active preparation tool checksum mismatch: ' + name)
    validate_ancestry(root, SERIES.parent, allow_missing=False)
    for patch in series['patches']:
        path = root / SERIES.parent / patch['path']
        validate_ancestry(root, SERIES.parent / patch['path'], allow_missing=False)
        if not path.is_file():
            raise PrepareError('patch is not a regular file: ' + str(path))
        if digest(path) != patch['sha256']:
            raise PrepareError('patch checksum mismatch: ' + patch['path'])


def scan_tree(root):
    if root.is_symlink() or not root.is_dir():
        raise PrepareError('tree root is not a regular directory: ' + str(root))
    files = {}
    directories = set()

    def visit(directory, prefix=''):
        with os.scandir(directory) as entries:
            for entry in entries:
                path = Path(entry.path)
                relative = prefix + entry.name
                if entry.is_symlink():
                    raise PrepareError('symbolic link in tree: ' + str(path))
                if entry.is_dir(follow_symlinks=False):
                    directories.add(relative)
                    visit(path, relative + '/')
                elif entry.is_file(follow_symlinks=False):
                    files[relative] = digest(path)
                else:
                    raise PrepareError('non-regular path in tree: ' + str(path))

    visit(root)
    return files, directories


def expected_tree(series):
    files = {output['path']: output['prepared_sha256'] for output in series['outputs']}
    directories = set()
    for name in files:
        parent = PurePosixPath(name).parent
        while parent != PurePosixPath('.'):
            directories.add(str(parent))
            parent = parent.parent
    return files, directories


def classify_prepared(root, series):
    validate_ancestry(root, PREPARED)
    prepared = root / PREPARED
    if not lexists(prepared):
        return 'absent'
    if not prepared.is_dir():
        raise PrepareError('prepared path is not a directory: ' + str(prepared))
    actual_files, actual_directories = scan_tree(prepared)
    expected_files, expected_directories = expected_tree(series)
    if actual_directories != expected_directories or set(actual_files) != set(expected_files):
        raise PrepareError(
            'prepared tree has missing, stale, or unknown paths; refusing to delete or publish over them')
    # Accept complete known snapshots only. Mixing base, frozen, previous
    # and current output bytes is not a recognized migration input.
    for state, key in (('prepared', 'prepared_sha256'), ('frozen', 'frozen_sha256'),
                       ('previous', 'previous_sha256'), ('base', 'base_sha256')):
        if actual_files == {output['path']: output[key] for output in series['outputs']}:
            return state
    for output in series['outputs']:
        actual = actual_files[output['path']]
        if actual not in (output['base_sha256'], output['frozen_sha256'],
                          output['previous_sha256'], output['prepared_sha256']):
            raise PrepareError('unrecognized or truncated prepared source: ' + output['path'])
    raise PrepareError('partial Moonlight prepared state; refusing publication')


def prepared_digests(root, series=None):
    series = series or load_series(root)
    if classify_prepared(root, series) != 'prepared':
        raise PrepareError('Moonlight prepared tree is not in the exact final state')
    return {output['path']: digest(root / PREPARED / output['path']) for output in series['outputs']}


def apply_patch(stage, patch):
    environment = git_environment()
    environment['GIT_CEILING_DIRECTORIES'] = str(stage.parent)
    for check_only in (True, False):
        args = ['git', 'apply', '--whitespace=nowarn']
        if check_only:
            args.append('--check')
        args.append(str(patch))
        result = subprocess.run(args, cwd=stage, env=environment, capture_output=True)
        if result.returncode:
            raise PrepareError(
                f'patch {"check" if check_only else "application"} failed for {patch.name}: '
                + result.stderr.decode(errors='replace').strip())


def stage_outputs(root, series, stage):
    repositories = repository_map(series)
    stage.mkdir()
    try:
        for output in series['outputs']:
            repository = repositories[output['repository']]
            source = root / repository['path']
            data = git_file(source, repository['revision'], output['source_path'])
            if digest_bytes(data) != output['base_sha256']:
                raise PrepareError('pinned Git object does not match base hash: ' + output['path'])
            target = stage / output['path']
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        for index, patch in enumerate(series['patches']):
            apply_patch(stage, root / SERIES.parent / patch['path'])
            actual_files, actual_directories = scan_tree(stage)
            _, expected_directories = expected_tree(series)
            if set(actual_files) != {output['path'] for output in series['outputs']} or actual_directories != expected_directories:
                raise PrepareError('patch touches paths outside the declared prepared tree: ' + patch['path'])
            if index == 7:
                for output in series['outputs']:
                    if actual_files[output['path']] != output['frozen_sha256']:
                        raise PrepareError('first eight patches differ from frozen C7 hash: ' + output['path'])
            if index == 8:
                for output in series['outputs']:
                    if actual_files[output['path']] != output['previous_sha256']:
                        raise PrepareError('first nine patches differ from 0.6.33 hash: ' + output['path'])
        actual_files, _ = scan_tree(stage)
        for output in series['outputs']:
            if actual_files[output['path']] != output['prepared_sha256']:
                raise PrepareError('prepared output hash mismatch: ' + output['path'])
    except Exception:
        if lexists(stage):
            shutil.rmtree(stage)
        raise


def remove_owned_tree(path):
    if lexists(path):
        if path.is_symlink() or not path.is_dir():
            raise PrepareError('owned publication path has unexpected type: ' + str(path))
        shutil.rmtree(path)


def publish_staged(root, stage, target, backup, state, validator):
    moved_old = False
    moved_new = False
    completed = False
    preserve = False
    try:
        if state != 'absent':
            target.rename(backup)
            moved_old = True
        stage.rename(target)
        moved_new = True
        validator()
        completed = True
    except Exception as error:
        rollback_errors = []
        if moved_new and lexists(target):
            try:
                target.rename(stage)
                moved_new = False
            except OSError as rollback_error:
                rollback_errors.append(f'{target} -> {stage}: {rollback_error}')
        if moved_old and lexists(backup) and not lexists(target):
            try:
                backup.rename(target)
                moved_old = False
            except OSError as rollback_error:
                rollback_errors.append(f'{backup} -> {target}: {rollback_error}')
        if rollback_errors:
            preserve = True
            raise PrepareError(
                'publication and rollback failed; recovery evidence preserved at '
                f'{stage}, {target}, {backup}: ' + '; '.join(rollback_errors)) from error
        raise
    finally:
        if not preserve:
            remove_owned_tree(stage)
            if completed or not moved_old:
                remove_owned_tree(backup)


def check_recovery_paths(root):
    for relative in (STAGING, BACKUP):
        validate_ancestry(root, relative)
        if lexists(root / relative):
            raise PrepareError(
                f'incomplete previous Moonlight publication; inspect recovery path {root / relative} '
                f'and prepared path {root / PREPARED}; automatic recovery is intentionally disabled')


def apply(root=ROOT, check=False):
    root = Path(root).absolute()
    series = load_series(root)
    validate_ancestry(root, BASE)
    (root / BASE).mkdir(parents=True, exist_ok=True)
    validate_ancestry(root, BASE, allow_missing=False)
    lock_path = root / BASE / '.prepare.lock'
    if lexists(lock_path) and not stat.S_ISREG(lock_path.lstat().st_mode):
        raise PrepareError('preparation lock is not a regular file: ' + str(lock_path))
    descriptor = os.open(lock_path, os.O_RDWR | os.O_CREAT | getattr(os, 'O_NOFOLLOW', 0), 0o600)
    with os.fdopen(descriptor, 'r+') as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        check_recovery_paths(root)
        validate_patch_inputs(root, series)
        validate_source_paths(root, series)
        validate_repositories(root, series)
        state = classify_prepared(root, series)
        if check and state != 'prepared':
            raise PrepareError('Moonlight prepared tree is not in the exact final state')
        stage = root / STAGING
        stage_outputs(root, series, stage)
        try:
            current = classify_prepared(root, series)
            if current != state:
                raise PrepareError('live Moonlight prepared tree changed during staging')
        except Exception:
            # Publication has not started. Discard only this invocation's new
            # stage; preserve the live tree and any concurrent user edits.
            remove_owned_tree(stage)
            raise
        if check or state == 'prepared':
            remove_owned_tree(stage)
            return 'checked' if check else 'unchanged'
        target = root / PREPARED
        backup = root / BACKUP
        publish_staged(
            root, stage, target, backup, state,
            lambda: prepared_digests(root, series))
        return 'prepared'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='validate exact final prepared bytes without publishing')
    args = parser.parse_args()
    try:
        result = apply(check=args.check)
        if result == 'unchanged':
            print('Moonlight patch series verified; exact prepared tree is a no-op')
        elif result == 'checked':
            print('Moonlight patch series and exact prepared tree verified')
        else:
            print('Moonlight patch series applied and verified: 24 files')
        return 0
    except (PrepareError, OSError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        print(f'Moonlight patch series rejected: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
