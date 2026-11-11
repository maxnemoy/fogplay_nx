#!/usr/bin/env python3
"""Validate and apply the pinned RTC patch series without partial source states."""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SERIES = Path('tools/rtc/patches/series.json')


class PatchError(RuntimeError):
    pass


def digest_bytes(data):
    return hashlib.sha256(data).hexdigest()


def digest(path):
    return digest_bytes(path.read_bytes())


def checked_path(value, label):
    path = PurePosixPath(value)
    if not value or path.is_absolute() or '..' in path.parts or str(path) != value:
        raise PatchError(f'unsafe {label}: {value!r}')
    return value


def load_series(root):
    try:
        data = json.loads((root / SERIES).read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise PatchError(f'cannot read series metadata: {error}') from error
    if data.get('schema') != 1 or not isinstance(data.get('repositories'), list):
        raise PatchError('unrecognized series schema')
    patches = set()
    for repository in data['repositories']:
        checked_path(repository['path'], 'repository path')
        checked_path(repository['patch'], 'patch path')
        if Path(repository['patch']).name != repository['patch']:
            raise PatchError('patch name must not contain directories')
        if repository['patch'] in patches:
            raise PatchError('duplicate patch in series: ' + repository['patch'])
        patches.add(repository['patch'])
        names = set()
        for entry in repository['files']:
            checked_path(entry['path'], 'source path')
            if entry['path'] in names:
                raise PatchError('duplicate source in series: ' + entry['path'])
            names.add(entry['path'])
            for key in ('base_sha256', 'patched_sha256'):
                value = entry[key]
                if value is not None and (key == 'patched_sha256' or len(value) != 64):
                    if not isinstance(value, str) or len(value) != 64:
                        raise PatchError(f'invalid {key} for {entry["path"]}')
        for submodule in repository.get('submodules', {}):
            checked_path(submodule, 'submodule path')
    actual = {path.name for path in (root / SERIES.parent).glob('*.patch')}
    if actual != patches:
        raise PatchError('patch directory and series metadata disagree')
    return data


def revision(repository):
    return subprocess.check_output(
        ['git', '-C', str(repository), 'rev-parse', 'HEAD'],
        stderr=subprocess.PIPE, text=True).strip()


def git_file(repository, commit, path):
    return subprocess.check_output(
        ['git', '-C', str(repository), 'show', f'{commit}:{path}'],
        stderr=subprocess.PIPE)


def names(output):
    return set(output.decode().split('\0')) - {''}


def local_changes(repository):
    # Git diff trusts these index flags and may hide modified compiled files.
    # A pinned source gate requires a complete view; never silently clear flags.
    indexed = subprocess.check_output(
        ['git', '-C', str(repository), 'ls-files', '-v', '-z'],
        stderr=subprocess.PIPE)
    hidden = [entry[2:].decode(errors='replace') for entry in indexed.split(b'\0')
              if entry and (entry[:1].islower() or entry[:1] == b'S')]
    if hidden:
        raise PatchError('hidden-index flags prevent source validation: ' + ', '.join(hidden))
    tracked = subprocess.check_output(
        ['git', '-C', str(repository), 'diff', '--name-only', 'HEAD', '-z'],
        stderr=subprocess.PIPE)
    untracked = subprocess.check_output(
        ['git', '-C', str(repository), 'ls-files', '--others', '--exclude-standard', '-z'],
        stderr=subprocess.PIPE)
    return names(tracked) | names(untracked)


def validate_inputs(root, series):
    for name, expected in series.get('tool_inputs', {}).items():
        checked_path(name, 'tool input path')
        if digest(root / name) != expected:
            raise PatchError('tool input changed without regenerating the series: ' + name)
    patches = root / SERIES.parent
    for repository in series['repositories']:
        patch = patches / repository['patch']
        if digest(patch) != repository['patch_sha256']:
            raise PatchError('patch checksum mismatch: ' + repository['patch'])


def validate_repositories(root, series):
    for repository in series['repositories']:
        source = root / repository['path']
        actual = revision(source)
        if actual != repository['revision']:
            raise PatchError(
                f'revision mismatch for {repository["path"]}: '
                f'expected {repository["revision"]}, got {actual}')
        for submodule, expected in repository.get('submodules', {}).items():
            actual = revision(source / submodule)
            if actual != expected:
                raise PatchError(
                    f'nested submodule revision mismatch for {submodule}: '
                    f'expected {expected}, got {actual}')
        allowed = {entry['path'] for entry in repository['files']}
        ignored = set(repository.get('submodules', {}))
        unknown = local_changes(source) - allowed - ignored
        if unknown:
            raise PatchError(
                f'unknown local source edits in {repository["path"]}: '
                + ', '.join(sorted(unknown)))


def validate_source_paths(root, series):
    if root.is_symlink():
        raise PatchError('RTC fixture root must not be a symbolic link: ' + str(root))
    for repository in series['repositories']:
        source = root
        for part in PurePosixPath(repository['path']).parts:
            source /= part
            if source.is_symlink():
                raise PatchError('symbolic link in RTC repository path: ' + str(source))
        for entry in repository['files']:
            path = source
            for part in PurePosixPath(entry['path']).parts:
                path /= part
                if path.is_symlink():
                    raise PatchError('symbolic link in RTC source path: ' + str(path))


def classify_sources(root, series):
    states = []
    for repository in series['repositories']:
        source = root / repository['path']
        for entry in repository['files']:
            path = source / entry['path']
            if path.is_symlink():
                raise PatchError('symbolic link in RTC source path: ' + str(path))
            if path.exists() and not path.is_file():
                raise PatchError('source is not a regular file: ' + str(path))
            actual = digest(path) if path.is_file() else None
            if actual == entry['base_sha256']:
                states.append('base')
            elif actual == entry['patched_sha256']:
                states.append('patched')
            else:
                raise PatchError('unrecognized or truncated source state: ' + str(path))
    if all(state == 'base' for state in states):
        return 'base'
    if all(state == 'patched' for state in states):
        return 'patched'
    raise PatchError('partial RTC patch state; restore all pristine bytes or all patched bytes')


def apply_patch(stage, patch):
    for check_only in (True, False):
        args = ['git', 'apply']
        if check_only:
            args.append('--check')
        args.append(str(patch))
        result = subprocess.run(args, cwd=stage, capture_output=True)
        if result.returncode:
            raise PatchError(
                f'patch {"check" if check_only else "application"} failed for {patch.name}: '
                + result.stderr.decode(errors='replace').strip())


def staged_outputs(root, series):
    outputs = {}
    with tempfile.TemporaryDirectory(prefix='fogplay-rtc-patches-') as directory:
        staging = Path(directory)
        for index, repository in enumerate(series['repositories']):
            source = root / repository['path']
            stage = staging / str(index)
            stage.mkdir()
            for entry in repository['files']:
                if entry['base_sha256'] is None:
                    continue
                data = git_file(source, repository['revision'], entry['path'])
                if digest_bytes(data) != entry['base_sha256']:
                    raise PatchError(
                        f'pinned Git object does not match base hash: '
                        f'{repository["path"]}/{entry["path"]}')
                path = stage / entry['path']
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            apply_patch(stage, root / SERIES.parent / repository['patch'])
            actual = {path.relative_to(stage).as_posix()
                      for path in stage.rglob('*') if path.is_file()}
            expected = {entry['path'] for entry in repository['files']}
            if actual != expected:
                raise PatchError('patch touches paths outside the declared file set: ' + repository['patch'])
            for entry in repository['files']:
                data = (stage / entry['path']).read_bytes()
                if digest_bytes(data) != entry['patched_sha256']:
                    raise PatchError(
                        f'patched output hash mismatch: {repository["path"]}/{entry["path"]}')
                outputs[(index, entry['path'])] = data
    return outputs


def install_outputs(root, series, outputs):
    created = []
    prepared = []
    published = []
    preserve_recovery_files = False
    try:
        validate_source_paths(root, series)
        for index, repository in enumerate(series['repositories']):
            for entry in repository['files']:
                target = root / repository['path'] / entry['path']
                target.parent.mkdir(parents=True, exist_ok=True)
                mode = target.stat().st_mode if target.exists() else 0o644
                backup_path = None
                if target.exists():
                    with tempfile.NamedTemporaryFile(dir=target.parent, delete=False) as backup:
                        backup_path = Path(backup.name)
                        created.append(backup_path)
                        backup.write(target.read_bytes())
                    os.chmod(backup_path, mode)
                with tempfile.NamedTemporaryFile(dir=target.parent, delete=False) as temporary:
                    temporary_path = Path(temporary.name)
                    created.append(temporary_path)
                    temporary.write(outputs[(index, entry['path'])])
                os.chmod(temporary_path, mode)
                prepared.append((temporary_path, target, backup_path))
        validate_source_paths(root, series)
        if classify_sources(root, series) != 'base':
            raise PatchError('live sources changed during patch preparation')
        for item in prepared:
            temporary, target, _ = item
            temporary.replace(target)
            published.append(item)
    except Exception as error:
        rollback_errors = []
        for temporary, target, backup in reversed(published):
            try:
                if backup is None:
                    target.unlink()
                else:
                    backup.replace(target)
            except OSError as rollback_error:
                rollback_errors.append(f'{target}: {rollback_error}')
        if rollback_errors:
            preserve_recovery_files = True
            raise PatchError(
                'publication failed and rollback was incomplete; recovery files preserved: '
                + '; '.join(rollback_errors)) from error
        raise
    finally:
        if not preserve_recovery_files:
            for path in created:
                if path.exists():
                    path.unlink()
    return len(prepared)


def apply(root=ROOT):
    series = load_series(root)
    validate_inputs(root, series)
    validate_source_paths(root, series)
    validate_repositories(root, series)
    state = classify_sources(root, series)
    outputs = staged_outputs(root, series)
    if state == 'patched':
        print('RTC patch series verified; exact patched state is a no-op')
        return 'unchanged'
    count = install_outputs(root, series, outputs)
    print(f'RTC patch series applied and verified: {count} files')
    return 'patched'


def main():
    try:
        apply()
    except (PatchError, OSError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        print(f'RTC patch series rejected: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
