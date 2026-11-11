#!/usr/bin/env python3
"""Materialize absent pins; never check out over an existing source worktree."""
from pathlib import Path
import subprocess

import prepare

URLS = {
    'switch': 'https://github.com/XITRIX/Moonlight-Switch.git',
    'common-c': 'https://github.com/XITRIX/moonlight-common-c.git',
    'enet': 'https://github.com/cgutman/enet.git',
    'nanors': 'https://github.com/sleepybishop/nanors.git',
}


def existing(root, repository):
    prepare.validate_ancestry(root, repository['path'])
    path = root / repository['path']
    if not prepare.lexists(path):
        return False
    if path.is_dir() and not any(path.iterdir()):
        return False  # Empty gitlink directory from the parent archive/clone.
    if not path.is_dir() or not prepare.lexists(path / '.git'):
        raise prepare.PrepareError('unrecognized existing source path: ' + str(path))
    actual = Path(prepare.command_text('git', '-C', str(path), 'rev-parse', '--show-toplevel'))
    if actual.resolve() != path.resolve():
        raise prepare.PrepareError('existing source Git worktree identity mismatch: ' + str(path))
    # Nested worktrees are validated individually below; absent ones are materialized later.
    if prepare.revision(path) != repository['revision']:
        raise prepare.PrepareError('existing source revision mismatch: ' + str(path))
    prepare.validate_index_flags(path)
    for args in (('diff', '--name-only', 'HEAD', '--ignore-submodules=all'),
                 ('ls-files', '--others')):
        if prepare.command_text('git', '-C', str(path), *args):
            raise prepare.PrepareError('existing source contains local edits: ' + str(path))
    return True


def fetch(root=prepare.ROOT):
    root = Path(root)
    series = prepare.load_series(root)
    prepare.validate_patch_inputs(root, series)
    # Reject every existing dirty/wrong/linked input before any clone or checkout.
    present = {repo['name']: existing(root, repo) for repo in series['repositories']}
    for repo in series['repositories']:
        if present[repo['name']]:
            continue
        source = root / repo['path']
        source.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['git', 'clone', '--no-checkout', URLS[repo['name']], str(source)],
                       check=True, env=prepare.git_environment())
        subprocess.run(['git', '-C', str(source), 'checkout', '--detach', repo['revision']],
                       check=True, env=prepare.git_environment())
    return prepare.apply(root)


if __name__ == '__main__':
    print(fetch())
