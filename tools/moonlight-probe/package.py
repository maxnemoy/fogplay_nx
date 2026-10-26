#!/usr/bin/env python3
"""Package only the local probe; never include saved configuration or identities."""
import argparse
import hashlib
import ipaddress
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[2]
VERSION = (ROOT / 'tools/moonlight-probe/VERSION').read_text().strip()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', required=True)
    parser.add_argument('--output', type=Path, default=ROOT / 'tools/moonlight-probe/dist')
    args = parser.parse_args()
    address = ipaddress.IPv4Address(args.host)
    if not any(address in ipaddress.IPv4Network(block) for block in ('10.0.0.0/8', '172.16.0.0/12', '192.168.0.0/16')):
        parser.error('host must be an RFC1918 LAN IPv4 address')
    binary = ROOT / 'tools/moonlight-probe/dist/moonlight_probe.nro'
    data = binary.read_bytes()
    if data[0x10:0x14] != b'NRO0':
        raise RuntimeError('Invalid NRO header')
    args.output.mkdir(parents=True, exist_ok=True)
    output = args.output / f'fogplay-moonlight-local-test-{VERSION}-sd.zip'
    prefix = 'switch/fogplay_nx/'
    config = dict(host=str(address), app_id=0, width=1280, height=720, fps=60, bitrate_kbps=10000)
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(prefix+'moonlight_probe.nro', data)
        archive.writestr(prefix+'moonlight-test.json', json.dumps(config, indent=2)+'\n')
        archive.write(ROOT / 'tools/moonlight-probe/README.md', prefix+'moonlight-test-README.md')
        archive.write(ROOT / 'third_party/moonlight/switch/build-manifest.json', prefix+'moonlight-test-licenses/build-manifest.json')
        for path in sorted((ROOT / 'licenses').rglob('*')):
            if path.is_file():archive.write(path, prefix+'moonlight-test-licenses/'+path.relative_to(ROOT / 'licenses').as_posix())
    with zipfile.ZipFile(output) as archive:
        assert all(name.startswith(prefix) and '..' not in Path(name).parts for name in archive.namelist())
        assert not any('identity' in name or name.endswith('auth.json') for name in archive.namelist())
        assert archive.read(prefix+'moonlight_probe.nro') == data
        assert json.loads(archive.read(prefix+'moonlight-test.json')) == config
    verification = dict(version=VERSION, sd_path=prefix.rstrip('/'), nro_sha256=hashlib.sha256(data).hexdigest(),
                        zip_sha256=hashlib.sha256(output.read_bytes()).hexdigest(), byte_size=len(data),
                        runtime_verified=False, rental_requests=False)
    (args.output/'moonlight-verification.json').write_text(json.dumps(verification, indent=2)+'\n')
    print(output)


if __name__ == '__main__':
    main()
