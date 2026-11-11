#!/usr/bin/env python3
"""Reject cached builds unless current sources, tools and artifacts all match."""
import json
import subprocess
import sys

import manifest
import prepare


def check():
    prefix = manifest.ROOT / 'third_party/moonlight/switch'
    recorded = json.loads((prefix / 'build-manifest.json').read_text())
    current = manifest.build_manifest(prefix)
    if recorded != current:
        raise ValueError('Moonlight build manifest is stale; rebuild through tools/moonlight/build.sh')


if __name__ == '__main__':
    try:
        check()
    except (OSError, KeyError, TypeError, ValueError, prepare.PrepareError,
            subprocess.CalledProcessError) as error:
        print(f'Moonlight cache rejected: {error}', file=sys.stderr)
        raise SystemExit(1)
