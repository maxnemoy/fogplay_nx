#!/usr/bin/env python3
"""Offline build regressions. All mutations are confined to temporary fixtures."""
import contextlib
import importlib.util
import io
import json
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
        self.write('src/core/constants/app_info.hpp',
                   '#define FOGPLAY_APP_VERSION "1.2.3"\n#define FOGPLAY_APP_AUTHOR "maxnemoy"\n')
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
        payload[0x80 + 56 + 0x3060:0x80 + 56 + 0x3065] = b'1.2.3'
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
            ('stale-version', 0x80 + 56 + 0x3060, b'1.2.2'),
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
        self.write('src/core/constants/app_info.hpp',
                   (ROOT / 'src/core/constants/app_info.hpp').read_text())
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

    def rtc_patch_fixture(self, root):
        module = load_module('rtc_patch_sources_test', ROOT / 'tools/rtc/patch-sources.py')
        shutil.copytree(ROOT / 'tools/rtc/patches', root / 'tools/rtc/patches')
        series = json.loads((root / 'tools/rtc/patches/series.json').read_text())
        for name in series['tool_inputs']:
            target = root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, target)
        base = {}
        expected = {}
        for repository in series['repositories']:
            for entry in repository['files']:
                source = ROOT / repository['path']
                target = root / repository['path'] / entry['path']
                key = (root / repository['path'], entry['path'])
                if entry['base_sha256'] is not None:
                    data = subprocess.check_output([
                        'git', '-C', str(source), 'show',
                        repository['revision'] + ':' + entry['path']])
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(data)
                    base[key] = data
                expected[key] = (source / entry['path']).read_bytes()
        return module, series, base, expected

    def rtc_real_git_fixture(self, root):
        module = load_module('rtc_patch_sources_real_git_test',
                             ROOT / 'tools/rtc/patch-sources.py')
        checker = load_module('rtc_source_checker_test',
                              ROOT / 'tools/rtc/check-source-patches.py')
        shutil.copytree(ROOT / 'tools/rtc/patches', root / 'tools/rtc/patches')
        series = json.loads((root / 'tools/rtc/patches/series.json').read_text())
        for name in series['tool_inputs']:
            target = root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, target)
        for repository in series['repositories']:
            target = root / repository['path']
            if target.exists():
                target.rmdir()
            checker.archive(ROOT / repository['path'], target)
            worktree = subprocess.check_output([
                'git', '-C', str(target), 'rev-parse', '--show-toplevel'], text=True).strip()
            self.assertEqual(Path(worktree), target)
        return module, series

    def run_rtc_patch(self, module, root, series, base,
                      wrong_revision=None, unknown_edit=False):
        def fake_revision(repository):
            repository = Path(repository)
            for entry in series['repositories']:
                if repository == root / entry['path']:
                    return '0' * 40 if wrong_revision == 'top' else entry['revision']
                for submodule, expected in entry.get('submodules', {}).items():
                    if repository == root / entry['path'] / submodule:
                        if wrong_revision == 'nested' and submodule.endswith('libjuice'):
                            return '0' * 40
                        return expected
            raise AssertionError(repository)

        def fake_changes(repository):
            if unknown_edit and Path(repository) == root / series['repositories'][0]['path']:
                return {'unexpected.cpp'}
            return set()

        with mock.patch.object(module, 'revision', side_effect=fake_revision), \
             mock.patch.object(module, 'local_changes', side_effect=fake_changes), \
             mock.patch.object(module, 'git_file',
                               side_effect=lambda repository, commit, path: base[(Path(repository), path)]):
            return module.apply(root)

    def rtc_source_tree(self, root):
        result = {}
        for path in (root / 'third_party').rglob('*'):
            if '.git' in path.parts:
                continue
            if path.is_symlink():
                result[path.relative_to(root).as_posix()] = ('symlink', os.readlink(path))
            elif path.is_file():
                result[path.relative_to(root).as_posix()] = ('file', path.read_bytes())
        return result

    def test_rtc_patch_series_pristine_replay_and_verified_noop(self):
        module, series, base, expected = self.rtc_patch_fixture(self.root)
        self.assertEqual(self.run_rtc_patch(module, self.root, series, base), 'patched')
        for (repository, name), data in expected.items():
            self.assertEqual((repository / name).read_bytes(), data)
        before = self.rtc_source_tree(self.root)
        timestamps = {path: path.stat().st_mtime_ns
                      for path in (self.root / 'third_party').rglob('*') if path.is_file()}
        self.assertEqual(self.run_rtc_patch(module, self.root, series, base), 'unchanged')
        self.assertEqual(self.rtc_source_tree(self.root), before)
        self.assertEqual({path: path.stat().st_mtime_ns for path in timestamps}, timestamps)

    def test_rtc_patch_series_failures_do_not_mutate_sources(self):
        cases = ('wrong-revision', 'wrong-nested-revision', 'changed-patch',
                 'changed-checksum', 'changed-source', 'truncated', 'partial', 'unknown-edit')
        for case in cases:
            with self.subTest(case=case):
                root = self.root / case
                module, series, base, expected = self.rtc_patch_fixture(root)
                repository = series['repositories'][0]
                entry = next(item for item in repository['files']
                             if item['base_sha256'] is not None)
                target = root / repository['path'] / entry['path']
                wrong_revision = None
                unknown_edit = False
                if case == 'wrong-revision':
                    wrong_revision = 'top'
                elif case == 'wrong-nested-revision':
                    wrong_revision = 'nested'
                elif case == 'changed-patch':
                    patch = root / 'tools/rtc/patches' / repository['patch']
                    patch.write_bytes(patch.read_bytes() + b'\n')
                elif case == 'changed-checksum':
                    repository['patch_sha256'] = '0' * 64
                    (root / 'tools/rtc/patches/series.json').write_text(json.dumps(series))
                elif case == 'changed-source':
                    target.write_bytes(b'unknown local edit\n')
                elif case == 'truncated':
                    target.write_bytes(b'')
                elif case == 'partial':
                    target.write_bytes(expected[(root / repository['path'], entry['path'])])
                elif case == 'unknown-edit':
                    unknown_edit = True
                before = self.rtc_source_tree(root)
                with self.assertRaises(module.PatchError):
                    self.run_rtc_patch(module, root, series, base,
                                       wrong_revision=wrong_revision, unknown_edit=unknown_edit)
                self.assertEqual(self.rtc_source_tree(root), before)

    def assert_rtc_patched(self, root, series, module):
        for repository in series['repositories']:
            for entry in repository['files']:
                path = root / repository['path'] / entry['path']
                self.assertEqual(module.digest(path), entry['patched_sha256'])
                self.assertEqual(path.read_bytes(),
                                 (ROOT / repository['path'] / entry['path']).read_bytes())

    def test_rtc_patch_series_real_git_replay_and_noop(self):
        root = self.root / 'real-git'
        module, series = self.rtc_real_git_fixture(root)
        self.assertEqual(module.apply(root), 'patched')
        self.assert_rtc_patched(root, series, module)
        before = self.rtc_source_tree(root)
        timestamps = {path: path.stat().st_mtime_ns
                      for path in (root / 'third_party').rglob('*')
                      if path.is_file() and '.git' not in path.parts}
        self.assertEqual(module.apply(root), 'unchanged')
        self.assertEqual(self.rtc_source_tree(root), before)
        self.assertEqual({path: path.stat().st_mtime_ns for path in timestamps}, timestamps)

    def test_rtc_real_git_nested_unknown_edit_is_rejected(self):
        root = self.root / 'real-git-nested-edit'
        module, series = self.rtc_real_git_fixture(root)
        repository = series['repositories'][1]
        target = root / repository['path'] / '.clang-format'
        target.write_bytes(target.read_bytes() + b'\nunknown nested edit\n')
        before = self.rtc_source_tree(root)
        with self.assertRaisesRegex(module.PatchError, 'unknown local source edits'):
            module.apply(root)
        self.assertEqual(self.rtc_source_tree(root), before)

    def test_rtc_publication_rename_failures_rollback_and_retry(self):
        for position in (1, 2, 19, 39):
            with self.subTest(position=position):
                root = self.root / f'publication-{position}'
                module, series = self.rtc_real_git_fixture(root)
                before = self.rtc_source_tree(root)
                original_replace = Path.replace
                calls = 0

                def fail_replace(path, target):
                    nonlocal calls
                    calls += 1
                    if calls == position:
                        raise OSError(f'injected rename failure at {position}')
                    return original_replace(path, target)

                with mock.patch.object(Path, 'replace', fail_replace):
                    with self.assertRaisesRegex(OSError, 'injected rename failure'):
                        module.apply(root)
                self.assertEqual(self.rtc_source_tree(root), before)
                self.assertEqual(module.apply(root), 'patched')
                self.assert_rtc_patched(root, series, module)

    def test_rtc_preparation_failures_clean_created_files_and_retry(self):
        for kind, position in (('open', 2), ('write', 1), ('write', 2),
                               ('chmod', 1), ('chmod', 2)):
            with self.subTest(kind=kind, position=position):
                root = self.root / f'preparation-{kind}-{position}'
                module, series = self.rtc_real_git_fixture(root)
                before = self.rtc_source_tree(root)
                calls = 0

                if kind == 'open':
                    original_open = module.tempfile.NamedTemporaryFile

                    def fail_open(*args, **kwargs):
                        nonlocal calls
                        calls += 1
                        if calls == position:
                            raise OSError(28, 'injected preparation open failure')
                        return original_open(*args, **kwargs)

                    injection = mock.patch.object(
                        module.tempfile, 'NamedTemporaryFile', side_effect=fail_open)
                elif kind == 'write':
                    original_open = module.tempfile.NamedTemporaryFile

                    def fail_write_open(*args, **kwargs):
                        nonlocal calls
                        calls += 1
                        handle = original_open(*args, **kwargs)
                        if calls == position:
                            def fail_write(data):
                                raise OSError(28, 'injected preparation write failure')
                            handle.write = fail_write
                        return handle

                    injection = mock.patch.object(
                        module.tempfile, 'NamedTemporaryFile', side_effect=fail_write_open)
                else:
                    original_chmod = module.os.chmod

                    def fail_chmod(path, mode):
                        nonlocal calls
                        calls += 1
                        if calls == position:
                            raise OSError(28, 'injected preparation chmod failure')
                        return original_chmod(path, mode)

                    injection = mock.patch.object(module.os, 'chmod', side_effect=fail_chmod)
                with injection:
                    with self.assertRaisesRegex(OSError, 'injected preparation'):
                        module.apply(root)
                self.assertEqual(self.rtc_source_tree(root), before)
                self.assertEqual(module.apply(root), 'patched')
                self.assert_rtc_patched(root, series, module)

    def test_rtc_declared_symlinks_and_parent_escapes_are_untouched(self):
        for case in ('declared', 'dangling-generated', 'parent-escape'):
            with self.subTest(case=case):
                root = self.root / f'symlink-{case}'
                module, series = self.rtc_real_git_fixture(root)
                repository = series['repositories'][0]
                if case == 'declared':
                    entry = repository['files'][0]
                    guarded = root / repository['path'] / entry['path']
                    external = root / 'external-source'
                    external.write_bytes(guarded.read_bytes())
                    guarded.unlink()
                    guarded.symlink_to(external)
                    external_before = external.read_bytes()
                elif case == 'dangling-generated':
                    entry = next(item for item in repository['files']
                                 if item['base_sha256'] is None)
                    guarded = root / repository['path'] / entry['path']
                    external = root / 'missing-target'
                    guarded.symlink_to(external)
                    external_before = None
                else:
                    entry = next(item for item in repository['files']
                                 if item['path'] == 'src/impl/init.cpp')
                    guarded = root / repository['path'] / 'src/impl'
                    external = root / 'external-impl'
                    shutil.move(str(guarded), str(external))
                    guarded.symlink_to(external, target_is_directory=True)
                    external_before = {path.relative_to(external): path.read_bytes()
                                       for path in external.rglob('*') if path.is_file()}
                before = self.rtc_source_tree(root)
                with self.assertRaisesRegex(module.PatchError, 'symbolic link'):
                    module.apply(root)
                self.assertEqual(self.rtc_source_tree(root), before)
                self.assertTrue(guarded.is_symlink())
                if case == 'declared':
                    self.assertEqual(external.read_bytes(), external_before)
                elif case == 'parent-escape':
                    self.assertEqual({path.relative_to(external): path.read_bytes()
                                      for path in external.rglob('*') if path.is_file()},
                                     external_before)
                else:
                    self.assertFalse(external.exists())

    def test_rtc_real_git_hidden_index_edits_rejected_without_mutation(self):
        for flag in ('assume-unchanged', 'skip-worktree'):
            with self.subTest(flag=flag):
                root = self.root / ('hidden-index-' + flag)
                module, series = self.rtc_real_git_fixture(root)
                repository = series['repositories'][1]
                worktree = root / repository['path']
                name = 'src/addr.c'
                self.assertNotIn(name, {entry['path'] for entry in repository['files']})
                source = worktree / name
                original = source.read_bytes()
                subprocess.run(['git', '-C', str(worktree), 'update-index', '--' + flag, name], check=True)
                source.write_bytes(original + b'\n/* hidden private fixture change */\n')
                before = self.rtc_source_tree(root)
                with self.assertRaisesRegex(module.PatchError, 'hidden-index flags'):
                    module.apply(root)
                self.assertEqual(self.rtc_source_tree(root), before)
                subprocess.run(['git', '-C', str(worktree), 'update-index', '--no-' + flag, name], check=True)
                with self.assertRaisesRegex(module.PatchError, 'unknown local source edits'):
                    module.apply(root)
                self.assertEqual(self.rtc_source_tree(root), before)
                source.write_bytes(original)
                self.assertEqual(module.apply(root), 'patched')
                self.assert_rtc_patched(root, series, module)

    def test_rtc_maintenance_lock_blocks_build_and_fetch_before_writes(self):
        self.write('tools/rtc/.typed-ice-integration.lock', 'explicit recovery fence')
        for name in ('build.sh', 'fetch.sh'):
            target = self.root / 'tools/rtc' / name
            shutil.copyfile(ROOT / 'tools/rtc' / name, target)
        before = {str(path.relative_to(self.root)): path.read_bytes()
                  for path in self.root.rglob('*') if path.is_file()}
        for name in ('build.sh', 'fetch.sh'):
            result = run('bash', str(self.root / 'tools/rtc' / name), 'host', cwd=self.root, ok=False)
            self.assertEqual(result.returncode, 1)
            self.assertIn('maintenance lock present', result.stderr)
            self.assertEqual({str(path.relative_to(self.root)): path.read_bytes()
                              for path in self.root.rglob('*') if path.is_file()}, before)

    def test_rtc_manifest_invalidates_recipes_sources_and_outputs(self):
        manifest = load_module('rtc_manifest', ROOT / 'tools/rtc/manifest.py')
        manifest.ROOT = self.root
        recipe = self.write('tools/rtc/CMakeLists.txt', 'recipe')
        series = self.write('tools/rtc/patches/series.json', 'series')
        patch = self.write('tools/rtc/patches/0001-libdatachannel.patch', 'patch')
        source = self.write('third_party/rtc/libdatachannel/source.cpp', 'source')
        self.write('third_party/rtc/mbedtls/source.cpp', 'tls source')
        self.write('devkit/cmake/Switch.cmake', 'toolchain')
        for platform in ('host', 'switch'):
            prefix = 'third_party/rtc/' + platform
            for name in manifest.LIBRARIES:
                self.write(prefix + '/lib/lib' + name + '.a', name)
            for name in ('rtc/rtc.hpp', 'rtc/icediagnostics.hpp', 'mbedtls/ssl.h', 'psa/crypto.h', 'mbedtls-user-config.h'):
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
                        series.write_text('changed series')
                        self.assertEqual(manifest.main(), 1)
                        series.write_text('series')
                        patch.write_text('changed patch')
                        self.assertEqual(manifest.main(), 1)
                        patch.write_text('patch')
                        self.assertEqual(manifest.main(), 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
