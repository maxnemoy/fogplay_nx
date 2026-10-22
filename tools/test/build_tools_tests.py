#!/usr/bin/env python3
"""Offline build regressions. All mutations are confined to temporary fixtures."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run(*args, cwd, ok=True, env=None):
    result = subprocess.run(args, cwd=cwd, env=env, text=True, capture_output=True)
    if ok and result.returncode:
        raise AssertionError(f'{args}: exit {result.returncode}\n{result.stdout}\n{result.stderr}')
    return result


class BuildToolsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='fogplay-build-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def write(self, name, content):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return path

    def package_fixture(self):
        shutil.copyfile(ROOT / 'package.py', self.root / 'package.py')
        self.write('Makefile', 'APP_VERSION := 1.2.3\n')
        self.write('README.md', 'test readme')
        self.write('docs/THIRD-PARTY.md', 'test licenses')
        self.write('licenses/example.txt', 'test license')
        self.write('dist/unrelated.bin', 'private artifact must not be packaged')
        payload = bytearray(0x80 + 56 + 0x4000 + 32)
        payload[0x10:0x14] = b'NRO0'
        struct.pack_into('<I', payload, 0x18, 0x80)
        payload[0x80:0x84] = b'ASET'
        struct.pack_into('<QQ', payload, 0x80 + 24, 56, 0x4000)
        payload[0x80 + 56 + 0x200:0x80 + 56 + 0x208] = b'maxnemoy'
        payload[-32:-15] = b'BEGIN CERTIFICATE'
        nro = self.root / 'dist/fogplay_nx.nro'
        nro.write_bytes(payload)
        return nro

    def test_optimized_python_rejects_corrupt_nro(self):
        nro = self.package_fixture()
        valid = nro.read_bytes()
        for label, offset, data in (
            ('truncated', 0, b''), ('nro', 0x10, b'BAD!'),
            ('asset', 0x80, b'BAD!'), ('author', 0x80 + 56 + 0x200, b'bad-name'),
            ('bounds', 0x80 + 24, struct.pack('<QQ', 2**63, 0x4000))):
            with self.subTest(label=label):
                payload = bytearray(valid)
                if label == 'truncated': payload = payload[:12]
                else: payload[offset:offset + len(data)] = data
                nro.write_bytes(payload)
                result = run(sys.executable, '-O', 'package.py', cwd=self.root, ok=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('Packaging failed:', result.stderr)
                self.assertFalse((self.root / 'dist/fogplay_nx-sd.zip').exists())

    def test_package_is_reproducible_and_preserves_allowlist(self):
        self.package_fixture()
        run(sys.executable, '-O', 'package.py', cwd=self.root)
        archive = self.root / 'dist/fogplay_nx-sd.zip'
        first = archive.read_bytes()
        for path in self.root.rglob('*'):
            if path.is_file(): os.utime(path, (1700000000, 1700000000))
        run(sys.executable, '-O', 'package.py', cwd=self.root)
        self.assertEqual(first, archive.read_bytes())
        import zipfile
        with zipfile.ZipFile(archive) as check:
            self.assertEqual(len(check.namelist()), 4)
            self.assertTrue(all(name.startswith('switch/fogplay_nx/') for name in check.namelist()))
            self.assertIn('switch/fogplay_nx/fogplay_nx.nro', check.namelist())
            self.assertFalse(any('unrelated' in name for name in check.namelist()))

    def test_make_tracks_system_headers_flags_and_safe_clean(self):
        shutil.copyfile(ROOT / 'Makefile', self.root / 'Makefile')
        self.write('fake-devkit/libnx/switch_rules', '')
        self.write('src/main.cpp', '#include <rtc-test.h>\nint value() { return RTC_VALUE; }\n')
        header = self.write('headers/rtc-test.h', '#define RTC_VALUE 1\n')
        self.write('work/preserve', 'keep')
        self.write('third_party/preserve', 'keep')
        self.write('dist/preserve.zip', 'keep')
        self.write('dist/fogplay_nx.nro', 'remove')
        self.write('dist/fogplay_nx-sd.zip', 'remove at distclean only')
        flags = '-std=c++17 -Wall -Werror -MD -MP -isystem ' + str(header.parent)
        args = ('make', 'build/main.o', 'DEVKITPRO=' + str(self.root / 'fake-devkit'),
                'CXX=g++', 'CXXFLAGS=' + flags)
        run(*args, cwd=self.root)
        obj = self.root / 'build/main.o'
        first = obj.stat().st_mtime_ns
        self.assertIn(str(header), (self.root / 'build/main.d').read_text())
        run(*args, cwd=self.root)
        self.assertEqual(first, obj.stat().st_mtime_ns)
        time.sleep(0.03)
        header.write_text('#define RTC_VALUE 2\n')
        run(*args, cwd=self.root)
        second = obj.stat().st_mtime_ns
        self.assertGreater(second, first)
        time.sleep(0.03)
        run(*args[:-1], 'CXXFLAGS=' + flags + ' -DNEW_FLAG=1', cwd=self.root)
        self.assertGreater(obj.stat().st_mtime_ns, second)
        run('make', 'clean', 'DEVKITPRO=', cwd=self.root)
        self.assertFalse(obj.exists())
        self.assertFalse((self.root / 'dist/fogplay_nx.nro').exists())
        self.assertTrue((self.root / 'dist/fogplay_nx-sd.zip').exists())
        run('make', 'distclean', 'DEVKITPRO=', cwd=self.root)
        for name in ('work/preserve', 'third_party/preserve', 'dist/preserve.zip'):
            self.assertEqual((self.root / name).read_text(), 'keep')
        self.assertFalse((self.root / 'dist/fogplay_nx-sd.zip').exists())

    def test_architecture_checks_nested_domain_files(self):
        self.write('tools/check-architecture.py', (ROOT / 'tools/check-architecture.py').read_text())
        self.write('src/features/example/domain/nested/deep/model.hpp', '#include <vector>\n')
        run(sys.executable, 'tools/check-architecture.py', cwd=self.root)
        self.write('src/features/example/domain/nested/deep/model.hpp', '#include "core/network/http.hpp"\n')
        result = run(sys.executable, 'tools/check-architecture.py', cwd=self.root, ok=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('nested/deep/model.hpp:1', result.stderr)

    def test_submodule_regex_accepts_only_error_prefixes(self):
        script = (ROOT / 'tools/rtc/fetch.sh').read_text()
        self.assertIn("'^[-+U]'", script)
        for prefix in ('-', '+', 'U'):
            result = subprocess.run(['grep', '-qE', '^[-+U]'], input=prefix + 'abc', text=True)
            self.assertEqual(result.returncode, 0)
        for prefix in (' ', 'A', '0'):
            result = subprocess.run(['grep', '-qE', '^[-+U]'], input=prefix + 'abc', text=True)
            self.assertEqual(result.returncode, 1)

    def test_rtc_failure_reports_actionable_log(self):
        self.write('tools/rtc/build.sh', (ROOT / 'tools/rtc/build.sh').read_text())
        fetch = self.write('tools/rtc/fetch.sh', '#!/bin/sh\necho fixture-fetch-failed\nexit 17\n')
        fetch.chmod(0o755)
        result = run('bash', 'tools/rtc/build.sh', 'host', cwd=self.root, ok=False)
        self.assertEqual(result.returncode, 17)
        self.assertIn('RTC host build failed (exit 17)', result.stderr)
        self.assertIn(str(self.root / 'work/rtc/fetch-host.log'), result.stderr)
        self.assertIn('fixture-fetch-failed', result.stderr)

    def test_rtc_manifest_invalidates_recipes_sources_and_outputs(self):
        manifest = load_module('rtc_manifest', ROOT / 'tools/rtc/manifest.py')
        manifest.ROOT = self.root
        recipe = self.write('tools/rtc/CMakeLists.txt', 'recipe')
        source = self.write('third_party/rtc/libdatachannel/source.cpp', 'source')
        self.write('third_party/rtc/mbedtls/source.cpp', 'tls source')
        self.write('devkit/cmake/Switch.cmake', 'toolchain')
        for platform in ('host', 'switch'):
            prefix = 'third_party/rtc/' + platform
            for name in manifest.LIBRARIES:
                self.write(prefix + '/lib/lib' + name + '.a', name)
            for name in ('rtc/rtc.hpp', 'mbedtls/ssl.h', 'psa/crypto.h', 'mbedtls-user-config.h'):
                self.write(prefix + '/include/' + name, name)
        with mock.patch.object(manifest, 'command', return_value='version-or-commit'), \
             mock.patch.object(manifest.subprocess, 'check_output', return_value=b'source.cpp\0'), \
             mock.patch.dict(os.environ, {'DEVKITPRO': str(self.root / 'devkit')}):
            for platform in ('host', 'switch'):
                with self.subTest(platform=platform), contextlib.redirect_stderr(io.StringIO()):
                    with mock.patch.object(sys, 'argv', ['manifest.py', 'write', platform]):
                        self.assertEqual(manifest.main(), 0)
                    with mock.patch.object(sys, 'argv', ['manifest.py', 'check', platform]):
                        self.assertEqual(manifest.main(), 0)
                        recipe.write_text('changed recipe')
                        self.assertEqual(manifest.main(), 1)
                        recipe.write_text('recipe')
                        source.write_text('modified source')
                        self.assertEqual(manifest.main(), 1)
                        source.write_text('source')
                        archive = self.root / 'third_party/rtc' / platform / 'lib/libjuice.a'
                        archive.write_text('corrupt')
                        self.assertEqual(manifest.main(), 1)
                        archive.write_text('juice')
                        self.assertEqual(manifest.main(), 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
