#!/usr/bin/env python3
"""Reapply all RTC recipes to pristine pinned source archives, without checkout changes."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def archive(source, target):
    target.mkdir(parents=True)
    process = subprocess.Popen(['git', '-C', str(source), 'archive', 'HEAD'], stdout=subprocess.PIPE)
    try:
        subprocess.run(['tar', '-xf', '-', '-C', str(target)], stdin=process.stdout, check=True)
    finally:
        process.stdout.close()
    if process.wait() != 0:
        raise RuntimeError('git archive failed: ' + str(source))


def digests(root):
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in root.rglob('*') if p.is_file()}


def main():
    work = ROOT / 'work/rtc'
    work.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='source-patches-', dir=work) as directory:
        fresh = Path(directory)
        rtc = Path('third_party/rtc/libdatachannel')
        archive(ROOT / rtc, fresh / rtc)
        # The patch recipes touch only these pinned submodules. Archive rather
        # than clone to exercise clean source bytes without network or refs.
        for submodule in ('deps/libjuice', 'deps/usrsctp'):
            target = fresh / rtc / submodule
            # Archive contains an empty submodule directory on some Git versions.
            if target.exists():
                target.rmdir()
            archive(ROOT / rtc / submodule, target)
        shutil.copytree(ROOT / 'tools/rtc', fresh / 'tools/rtc',
                        ignore=shutil.ignore_patterns('__pycache__'))
        original = digests(fresh / rtc)
        recipe = fresh / 'tools/rtc/patch-sources.py'
        subprocess.run(['python3', str(recipe)], check=True)
        once = digests(fresh / rtc)
        subprocess.run(['python3', str(recipe)], check=True)
        assert once == digests(fresh / rtc), 'second patch application changes sources'
        changed = [name for name, digest in once.items() if original.get(name) != digest]
        for name in changed:
            assert (fresh / rtc / name).read_bytes() == (ROOT / rtc / name).read_bytes(), name
        print(f'Fresh pinned-source patches: {len(changed)} changed/generated files match live tree; second application byte-identical PASS')


if __name__ == '__main__':
    main()
