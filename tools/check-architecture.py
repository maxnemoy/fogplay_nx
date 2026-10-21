#!/usr/bin/env python3
"""Check that domain files do not include platform or external adapter headers."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
violations = []
for path in sorted((root / 'src/features').rglob('*')):
    if not path.is_file() or 'domain' not in path.relative_to(root / 'src/features').parts[:-1]: continue
    if path.suffix not in ('.h', '.hpp', '.c', '.cpp', '.cc', '.cxx'): continue
    for line, text in enumerate(path.read_text().splitlines(), 1):
        match = re.match(r'\s*#include\s*[<"]([^>"\n]+)', text)
        if not match: continue
        dependency = match.group(1)
        forbidden = ('/infrastructure/', '/presentation/', '/application/', 'platform/',
                     'app/', 'core/', 'SDL', 'switch.h', 'curl/', 'rtc/', 'libav',
                     'fcntl.h', 'unistd.h', 'sys/stat.h', 'fstream')
        if any(part in dependency for part in forbidden):
            violations.append(f'{path.relative_to(root)}:{line}: {dependency}')
if violations:
    raise SystemExit('Domain dependency violations:\n' + '\n'.join(violations))
print('Domain dependency boundaries passed')
