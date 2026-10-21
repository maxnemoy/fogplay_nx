#!/usr/bin/env python3
"""Rebuild a pinned NVTEGRA object; never write to the global installation."""
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/ffmpeg'
WORK = ROOT / 'work/ffmpeg-diag'
OUTPUT = ROOT / 'third_party/ffmpeg-diag/switch/lib/libavcodec.a'
MANIFEST = OUTPUT.parent / 'manifest.json'
INSTALLED = Path(os.environ['PORTLIBS_PREFIX']) / 'lib/libavcodec.a'
AR = os.environ['AR']
CC = os.environ['CC']
EXPECTED = {
    'ffmpeg-7.1.tar.xz': '40973d44970dbc83ef302b0609f2e74982be2d85916dd2ee7472d30678a7abe6',
    'ffmpeg-7.1-devkitpro.patch': '1792380b992e3554a4abcddf0d7b395bfd8c118ac7c6e38c8f2fb0d39753a390',
    'tls.patch': '57ea7ec8ed26d13d3172d0fd589b7883d22f0c180d50b7434fcc73fb2b3ab7d7',
}

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def command(args, cwd=None):
    return subprocess.check_output(args, cwd=cwd, text=True).strip()

def archive_objects(path):
    # GNU ar metadata tables may change when replacing a member; every actual
    # unmodified object must remain byte-for-byte identical.
    blob = path.read_bytes()
    if blob[:8] != b'!<arch>\n':
        raise RuntimeError('Expected standard ar archive')
    position = 8
    names = b''
    objects = []
    while position < len(blob):
        header = blob[position:position + 60]
        if len(header) != 60 or header[58:60] != b'`\n':
            raise RuntimeError('Malformed archive member')
        size = int(header[48:58].strip())
        data = blob[position + 60:position + 60 + size]
        name = header[:16].decode().strip()
        position += 60 + size + size % 2
        if name == '//':
            names = data
            continue
        if name in ('/', '/SYM64/'):
            continue
        if name.startswith('/'):
            offset = int(name[1:])
            name = names[offset:names.index(b'/\n', offset)].decode()
        else:
            name = name.rstrip('/')
        # FFmpeg has legitimate duplicate basenames from arch-specific paths.
        objects.append((name, hashlib.sha256(data).hexdigest()))
    return objects

def main():
    WORK.mkdir(parents=True, exist_ok=True)
    tarball = WORK / 'ffmpeg-7.1.tar.xz'
    if not tarball.exists():
        with urllib.request.urlopen('https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz', timeout=60) as response:
            tarball.write_bytes(response.read())
    for name, expected in EXPECTED.items():
        path = tarball if name.endswith('.tar.xz') else TOOLS / name
        if digest(path) != expected:
            raise RuntimeError(f'Pinned input checksum mismatch: {path}')
    configurations = [line for line in command(['strings', str(INSTALLED)]).splitlines() if line.startswith('--prefix=')]
    if len(configurations) != 1:
        raise RuntimeError('Expected exactly one installed FFmpeg configuration')
    config = configurations[0]
    if '--enable-nvtegra' not in config or '--target-os=horizon' not in config:
        raise RuntimeError('Installed FFmpeg is not the expected Switch NVTEGRA build')
    version = command(['pkg-config', '--modversion', 'libavcodec'], cwd=ROOT)
    if version != '61.19.100':
        raise RuntimeError(f'Expected FFmpeg 7.1 libavcodec 61.19.100, got {version}')
    inputs = {name: expected for name, expected in EXPECTED.items()}
    inputs.update(installed_archive=digest(INSTALLED), configuration=config,
                  compiler=command([CC, '--version']), ar=command([AR, '--version']),
                  diagnostic_patch=digest(TOOLS / 'nvdec-diagnostics.patch'),
                  builder=digest(Path(__file__)))
    if OUTPUT.exists() and MANIFEST.exists():
        manifest = json.loads(MANIFEST.read_text())
        if manifest.get('inputs') == inputs and manifest.get('output_sha256') == digest(OUTPUT):
            print('FFmpeg NVDEC diagnostic overlay verified (cached).')
            return
    # Fresh isolated extraction avoids any stale objects/configuration.
    with tempfile.TemporaryDirectory(prefix='build-', dir=WORK) as directory:
        stage = Path(directory)
        subprocess.run(['tar', '-xf', str(tarball), '-C', str(stage)], check=True)
        source = stage / 'ffmpeg-7.1'
        for patch in ['ffmpeg-7.1-devkitpro.patch', 'tls.patch', 'nvdec-diagnostics.patch']:
            subprocess.run(['patch', '-s', '-p1', '-i', str(TOOLS / patch)], cwd=source, check=True)
        # Reject public ABI/header drift before mixing a replacement object into
        # the installed archive. Internal source comes from the pinned patch.
        for name in ['libavcodec/avcodec.h', 'libavcodec/codec.h', 'libavutil/hwcontext_nvtegra.h', 'libavutil/nvtegra.h']:
            installed_header = Path(os.environ['PORTLIBS_PREFIX']) / 'include' / name
            if digest(source / name) != digest(installed_header):
                raise RuntimeError(f'Installed public header differs from pinned source: {name}')
        subprocess.run(['./configure', *shlex.split(config)], cwd=source, check=True,
                       stdout=open(WORK / 'configure.log', 'w'), stderr=subprocess.STDOUT)
        subprocess.run(['make', '-j2', 'libavcodec/nvtegra_decode.o'], cwd=source, check=True,
                       stdout=open(WORK / 'object-build.log', 'w'), stderr=subprocess.STDOUT)
        obj = source / 'libavcodec/nvtegra_decode.o'
        strings = command(['strings', str(obj)])
        for marker in ['FOG_NVDEC_STATUS_V1', 'FOG_NVDEC_WAIT_V1']:
            if marker not in strings:
                raise RuntimeError(f'Missing diagnostic marker: {marker}')
        OUTPUT.parent.mkdir(parents=True, exist_ok=True)
        temporary = stage / 'libavcodec.a'
        temporary.write_bytes(INSTALLED.read_bytes())
        subprocess.run([AR, 'r', str(temporary), str(obj)], check=True)
        subprocess.run([AR, 's', str(temporary)], check=True)
        original_members = command([AR, 't', str(INSTALLED)]).splitlines()
        if command([AR, 't', str(temporary)]).splitlines() != original_members:
            raise RuntimeError('Replacement changed archive member list')
        if original_members.count('nvtegra_decode.o') != 1:
            raise RuntimeError('Expected one NVTEGRA decoder member')
        originals = archive_objects(INSTALLED)
        replacements = archive_objects(temporary)
        if [(name, sha) for name, sha in originals if name != 'nvtegra_decode.o'] != [
                (name, sha) for name, sha in replacements if name != 'nvtegra_decode.o']:
            raise RuntimeError('An unmodified archive object changed')
        extracted = subprocess.check_output([AR, 'p', str(temporary), 'nvtegra_decode.o'])
        if extracted != obj.read_bytes():
            raise RuntimeError('Overlay member does not match rebuilt object')
        # Copy generated configuration and object for reproducibility/evidence.
        for name in ['config.h', 'config_components.h', 'ffbuild/config.mak']:
            (WORK / Path(name).name).write_bytes((source / name).read_bytes())
        (WORK / 'nvtegra_decode.o').write_bytes(obj.read_bytes())
        temporary.replace(OUTPUT)
        MANIFEST.write_text(json.dumps({'inputs': inputs, 'output_sha256': digest(OUTPUT),
                                       'object_sha256': digest(obj),
                                       'unmodified_objects_verified': len(originals) - 1}, indent=2) + '\n')
    print(f'Built FFmpeg NVDEC diagnostic overlay: {OUTPUT}')

if __name__ == '__main__':
    main()
