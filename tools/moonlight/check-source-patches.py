#!/usr/bin/env python3
"""Replay the Moonlight series in isolated real-Git pristine worktrees."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

import prepare

ROOT = Path(__file__).resolve().parents[2]


def archive(source, target, revision):
    target.mkdir(parents=True, exist_ok=True)
    environment = prepare.git_environment()
    process = subprocess.Popen(['git', '-C', str(source), 'archive', revision],
                               stdout=subprocess.PIPE, env=environment)
    try:
        subprocess.run(['tar', '-xf', '-', '-C', str(target)], stdin=process.stdout, check=True)
    finally:
        process.stdout.close()
    if process.wait() != 0:
        raise RuntimeError('git archive failed: ' + str(source))
    origin = Path(subprocess.check_output(
        ['git', '-C', str(source), 'rev-parse', '--absolute-git-dir'], text=True, env=environment).strip())
    gitdir = target / '.git'
    (gitdir / 'objects/info').mkdir(parents=True)
    (gitdir / 'refs').mkdir()
    (gitdir / 'HEAD').write_text(revision + '\n')
    (gitdir / 'config').write_text(
        '[core]\n repositoryformatversion = 0\n bare = false\n worktree = ' + str(target) + '\n')
    (gitdir / 'objects/info/alternates').write_text(str(origin / 'objects') + '\n')
    subprocess.run(['git', '-C', str(target), 'read-tree', 'HEAD'], check=True, env=environment)
    worktree = Path(subprocess.check_output(
        ['git', '-C', str(target), 'rev-parse', '--show-toplevel'], text=True, env=environment).strip())
    if worktree.resolve() != target.resolve():
        raise RuntimeError('temporary Git worktree escaped its archive: ' + str(target))


def source_digests(root):
    return {
        path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in root.rglob('*')
        if path.is_file() and '.git' not in path.relative_to(root).parts
    }


def main():
    series = json.loads((ROOT / 'tools/moonlight/patches/series.json').read_text())
    work = ROOT / 'work/moonlight'
    work.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='source-patches-', dir=work) as directory:
        fresh = Path(directory)
        source_root = fresh / 'third_party/moonlight'
        for repository in series['repositories']:
            relative = Path(repository['path']).relative_to('third_party/moonlight')
            archive(ROOT / repository['path'], source_root / relative, repository['revision'])
        shutil.copytree(ROOT / 'tools/moonlight', fresh / 'tools/moonlight',
                        ignore=shutil.ignore_patterns('__pycache__'))
        before = source_digests(source_root / 'moonlight-switch')
        recipe = fresh / 'tools/moonlight/prepare.py'
        subprocess.run([sys.executable, '-B', str(recipe)], check=True)
        prepared = source_root / 'prepared'
        once = source_digests(prepared)
        mtimes = {path: path.stat().st_mtime_ns for path in prepared.rglob('*') if path.is_file()}
        subprocess.run([sys.executable, '-B', str(recipe)], check=True)
        assert once == source_digests(prepared), 'second preparation changed bytes'
        assert mtimes == {path: path.stat().st_mtime_ns for path in prepared.rglob('*') if path.is_file()}
        assert before == source_digests(source_root / 'moonlight-switch'), 'pinned worktree bytes changed'
        expected = {output['path']: output['prepared_sha256'] for output in series['outputs']}
        assert once == expected, 'prepared output differs from checksummed series'
        aggregate = hashlib.sha256()
        for name in sorted(once):
            aggregate.update(name.encode() + b'\0' + once[name].encode() + b'\n')
        print(f'Moonlight isolated real-Git replay: {len(once)} files match series; '
              f'repeat no-op preserves bytes/mtimes; aggregate {aggregate.hexdigest()} PASS')


if __name__ == '__main__':
    main()
