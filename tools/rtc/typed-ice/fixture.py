"""Read pinned Git objects; never mutate the source repos or their indexes."""
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import tarfile

SERIES_SHA256 = '57ad9716d1670afcb58145690c3bab712f13673c8da455bfba995041e9d247da'
BASELINE = Path(__file__).resolve().parent / 'baseline'
MBEDTLS_REVISION = 'c765c831e5c2a0971410692f92f7a81d6ec65ec2'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(*args, cwd=None):
    return subprocess.check_output([str(a) for a in args], cwd=cwd)


def archive(source, revision, destination, revisions):
    destination.mkdir(parents=True, exist_ok=True)
    revisions[str(source)] = revision
    data = command('git', '-C', source, 'archive', revision)
    with tarfile.open(fileobj=io.BytesIO(data)) as tar:
        tar.extractall(destination, filter='data')
    entries = command('git', '-C', source, 'ls-tree', '-rz', revision)
    for entry in entries.split(b'\0'):
        if not entry:
            continue
        info, name = entry.split(b'\t', 1)
        mode, kind, oid = info.decode().split()
        if kind == 'commit':
            name = name.decode()
            archive(source / name, oid, destination / name, revisions)


def prepare(root, work):
    if work.exists():
        raise RuntimeError('Private work directory must not exist: ' + str(work))
    series_path = BASELINE / 'patches/series.json'
    if sha(series_path) != SERIES_SHA256:
        raise RuntimeError('Approved series baseline changed; review required')
    series = json.loads(series_path.read_text())
    work.mkdir(parents=True)
    revisions = {}
    sdk = work / 'third_party/rtc/libdatachannel'
    archive(root / series['repositories'][0]['path'],
            series['repositories'][0]['revision'], sdk, revisions)
    archive(root / 'third_party/rtc/mbedtls', MBEDTLS_REVISION,
            work / 'third_party/rtc/mbedtls', revisions)
    for repo in series['repositories']:
        patch = BASELINE / 'patches' / repo['patch']
        assert sha(patch) == repo['patch_sha256'], patch
        dest = work / repo['path']
        for item in repo['files']:
            path = dest / item['path']
            assert (sha(path) if path.exists() else None) == item['base_sha256'], path
        command('git', 'apply', '--check', patch, cwd=dest)
        command('git', 'apply', patch, cwd=dest)
        for item in repo['files']:
            assert sha(dest / item['path']) == item['patched_sha256'], item
    for name, expected in series['tool_inputs'].items():
        assert sha(BASELINE / 'tool-inputs' / name) == expected, name
    # Copy build inputs into the PRIVATE layout; do not run normal build.sh.
    shutil.copytree(root / 'tools/rtc', work / 'tools/rtc',
                    ignore=shutil.ignore_patterns('typed-ice', 'patches', '__pycache__'))
    shutil.copytree(BASELINE / 'patches', work / 'tools/rtc/patches')
    for name in series['tool_inputs']:
        shutil.copyfile(BASELINE / 'tool-inputs' / name, work / name)
    # The normal recipe may now declare the permanent typed test target.
    test = work / 'tools/rtc/typed-ice/typed-ice-test.cpp'
    test.parent.mkdir(parents=True)
    shutil.copyfile(Path(__file__).resolve().parent / 'typed-ice-test.cpp', test)
    baseline = {str(p.relative_to(sdk)): sha(p) for p in sdk.rglob('*') if p.is_file()}
    evidence = {'series_sha256': SERIES_SHA256, 'revisions': revisions,
                'baseline_files': baseline,
                'recipe_inputs': {str(p.relative_to(work)): sha(p)
                    for p in (work / 'tools/rtc').rglob('*') if p.is_file()}}
    (work / 'baseline.json').write_text(json.dumps(evidence, indent=2, sort_keys=True) + '\n')
    return sdk, evidence
