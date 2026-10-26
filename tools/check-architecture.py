#!/usr/bin/env python3
"""Enforce pure domain models and keep provider protocols out of use cases/UI."""
from pathlib import Path
import re
root = Path(__file__).resolve().parents[1]
source = root / 'src'
violations = []
for path in sorted(source.rglob('*')):
    if not path.is_file() or path.suffix not in ('.h', '.hpp', '.c', '.cpp', '.cc', '.cxx'): continue
    parts = path.relative_to(source).parts[:-1]
    domain = 'domain' in parts
    consumer = domain or 'application' in parts or 'presentation' in parts or parts == ('app',)
    if not consumer: continue
    for line, text in enumerate(path.read_text().splitlines(), 1):
        match = re.match(r'\s*#include\s*[<"]([^>"\n]+)', text)
        dependency = match.group(1) if match else ''
        forbidden = ('nlohmann/', 'core/network/', '/infrastructure/api.hpp', '/infrastructure/mts_', '/infrastructure/input_protocol.hpp')
        invalid = any(part in dependency for part in forbidden)
        if domain:
            adapters = ('/infrastructure/', '/presentation/', '/application/', 'platform/', 'app/',
                        'SDL', 'switch.h', 'curl/', 'rtc/', 'libav', 'fcntl.h', 'unistd.h', 'sys/stat.h', 'fstream')
            invalid = invalid or any(part in dependency for part in adapters)
            invalid = invalid or (dependency.startswith('core/') and not dependency.startswith('core/domain/'))
        # A forward declaration/alias must not hide a provider payload at a boundary.
        invalid = invalid or bool(re.search(r'\bnlohmann\b|\bJson\b|\bHttpClient\b',text.split('//',1)[0]))
        if invalid: violations.append(f'{path.relative_to(root)}:{line}: {text.strip()}')
if violations: raise SystemExit('Architecture boundary violations:\n' + '\n'.join(violations))
print('Domain and provider boundary checks passed')
