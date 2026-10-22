#!/usr/bin/env python3
"""Package only explicit release files; never copy SD account data or old artifacts."""
import hashlib
import json
from pathlib import Path
import re
import struct
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parent

def require(condition, message):
    if not condition:
        raise ValueError(message)

def main():
    nro = ROOT / 'dist/fogplay_nx.nro'
    payload = nro.read_bytes()
    require(len(payload) >= 0x80 and payload[0x10:0x14] == b'NRO0', 'Missing NRO header')
    asset_offset = struct.unpack_from('<I', payload, 0x18)[0]
    require(0x80 <= asset_offset <= len(payload) - 56, 'Invalid NRO asset offset')
    require(payload[asset_offset:asset_offset + 4] == b'ASET', 'Missing NRO assets')
    require(b'BEGIN CERTIFICATE' in payload, 'Missing embedded CA bundle')
    nacp_offset, nacp_size = struct.unpack_from('<QQ', payload, asset_offset + 24)
    require(nacp_offset >= 56 and nacp_size >= 0x4000
            and nacp_offset + nacp_size <= len(payload) - asset_offset,
            'Invalid NACP bounds')
    nacp = payload[asset_offset + nacp_offset:asset_offset + nacp_offset + nacp_size]
    author = nacp[0x200:0x300].split(b'\0', 1)[0].decode('utf-8')
    require(author == 'maxnemoy', f'Unexpected homebrew author: {author}')
    version_match = re.search(r'^APP_VERSION := (\d+\.\d+\.\d+)$',
                              (ROOT / 'Makefile').read_text(), re.M)
    require(version_match is not None, 'APP_VERSION is missing or invalid in Makefile')
    version = version_match.group(1)
    install = 'switch/fogplay_nx'
    files = {'fogplay_nx.nro': nro, 'README.md': ROOT / 'README.md',
             'docs/THIRD-PARTY.md': ROOT / 'docs/THIRD-PARTY.md'}
    if (ROOT / 'LICENSE').exists(): files['LICENSE'] = ROOT / 'LICENSE'
    for p in sorted((ROOT / 'licenses').rglob('*')):
        if p.is_file(): files['licenses/' + p.relative_to(ROOT / 'licenses').as_posix()] = p
    manifest = ROOT / 'third_party/ffmpeg-diag/switch/lib/manifest.json'
    if manifest.exists(): files['licenses/ffmpeg-build-manifest.json'] = manifest
    archive = ROOT / 'dist/fogplay_nx-sd.zip'
    with tempfile.TemporaryDirectory(dir=ROOT / 'dist', prefix='package-') as temp:
        stage = Path(temp)
        temporary_zip = stage / 'release.zip'
        with zipfile.ZipFile(temporary_zip, 'w', zipfile.ZIP_DEFLATED) as out:
            for name, source in sorted(files.items()):
                info = zipfile.ZipInfo(install + '/' + name, date_time=(1980, 1, 1, 0, 0, 0))
                info.create_system = 3
                info.external_attr = 0o100644 << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                out.writestr(info, source.read_bytes())
        with zipfile.ZipFile(temporary_zip) as check:
            require(check.testzip() is None, 'Release ZIP integrity check failed')
            require(check.read(install + '/fogplay_nx.nro') == payload,
                    'Release ZIP NRO differs from input')
            require(set(check.namelist()) == {install + '/' + name for name in files},
                    'Release ZIP contains unexpected or missing files')
        temporary_zip.replace(archive)
    result = {'version': version, 'author': author, 'install_folder': install,
              'nro_sha256': hashlib.sha256(payload).hexdigest(), 'nro_bytes': len(payload),
              'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
              'zip_integrity': True, 'nro_header': True, 'asset_header': True,
              'ca_bundle_embedded': True, 'switch_runtime_tested': False}
    (ROOT / 'dist/verification.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))

if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, struct.error) as error:
        raise SystemExit(f'Packaging failed: {error}')
